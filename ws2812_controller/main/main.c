#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      18


#define WIFI_CONNECTED_BIT BIT0
#define PORT 3333

static const char *TAG = "example";


// Dynamic number of LEDs (adjust this value as needed)
static uint32_t EXAMPLE_LED_NUMBERS = 8; // You can dynamically change this value
float g_led_brightness = 0.2f; // Range: 0.0 (off) to 1.0 (full brightness)
int EXAMPLE_CHASE_SPEED_MS = 50;

static EventGroupHandle_t wifi_event_group;
// Array to store LED color values (RGB for each LED)
static uint8_t *led_strip_pixels = NULL;
typedef struct {
    char ssid[32];
    char password[64];
} wifi_creds_t;



void apply_global_brightness(uint8_t *pixels, size_t num_leds)
{
    for (size_t i = 0; i < num_leds * 3; i++) {
        pixels[i] = (uint8_t)(pixels[i] * g_led_brightness);
    }
}


static void save_wifi_creds_to_nvs(const wifi_creds_t *creds) {
    nvs_handle_t nvs;
    if (nvs_open("wifi_creds", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", creds->ssid);
        nvs_set_str(nvs, "password", creds->password);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved WiFi credentials to NVS");
    }
}

// NVS Read
static bool read_wifi_creds_from_nvs(wifi_creds_t *creds) {
    nvs_handle_t nvs;
    size_t ssid_len = sizeof(creds->ssid);
    size_t pass_len = sizeof(creds->password);

    if (nvs_open("wifi_creds", NVS_READONLY, &nvs) != ESP_OK) return false;
    if (nvs_get_str(nvs, "ssid", creds->ssid, &ssid_len) != ESP_OK ||
        nvs_get_str(nvs, "password", creds->password, &pass_len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    nvs_close(nvs);
    return true;
}

// Event Handler for STA mode
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// STA Mode Connect
static bool connect_to_wifi(const wifi_creds_t *creds) {
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_event_group = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, creds->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, creds->password, sizeof(wifi_config.sta.password));
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to WiFi...");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    return bits & WIFI_CONNECTED_BIT;
}

// AP Mode Start
static void start_ap_mode() {
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32_PROV",
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 1,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP started. SSID: ESP32_PROV, Port: %d", PORT);
}

// Switch back to STA mode with new creds
static void reconnect_sta_mode(const wifi_creds_t *creds) {
    ESP_LOGI(TAG, "Stopping AP mode...");
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_deinit();

    ESP_LOGI(TAG, "Switching to STA mode...");
    if (connect_to_wifi(creds)) {
        ESP_LOGI(TAG, "Connected to new WiFi credentials!");
    } else {
        ESP_LOGE(TAG, "Failed to connect with new credentials.");
    }
}

// TCP Server in AP mode
static void tcp_server_task(void *pvParameter) {
    char rx_buffer[128];
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(listen_sock, 1);

    ESP_LOGI(TAG, "Waiting for client...");

    int sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
    if (sock < 0) {
        ESP_LOGE(TAG, "Accept failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
    if (len > 0) {
        rx_buffer[len] = '\0';
        ESP_LOGI(TAG, "Received: %s", rx_buffer);

        wifi_creds_t creds = { 0 };
        sscanf(rx_buffer, "%31[^,],%63s", creds.ssid, creds.password);
        save_wifi_creds_to_nvs(&creds);

        char ack[] = "Credentials received";
        send(sock, ack, strlen(ack), 0);
        shutdown(sock, 0);
        close(sock);
        close(listen_sock);

        reconnect_sta_mode(&creds);
    } else {
        ESP_LOGW(TAG, "No data received");
        shutdown(sock, 0);
        close(sock);
        close(listen_sock);
    }

    vTaskDelete(NULL);
}



/**
 * @brief Simple helper function, converting HSV color space to RGB color space
 */
void led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360; // h -> [0,360]
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    // RGB adjustment amount by hue
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

void rainbow_chase(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    uint16_t hue = 0;
    uint16_t start_rgb = 0;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // no transfer loop
    };

    while (1) {
        for (int i = 0; i < 3; i++) {
            for (int j = i; j < EXAMPLE_LED_NUMBERS; j += 3) {
                // Build RGB pixels
                hue = j * 360 / EXAMPLE_LED_NUMBERS + start_rgb;
                led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
                led_strip_pixels[j * 3 + 0] = green;
                led_strip_pixels[j * 3 + 1] = blue;
                led_strip_pixels[j * 3 + 2] = red;
            }

            // Apply global brightness before sending the data
            apply_global_brightness(led_strip_pixels, EXAMPLE_LED_NUMBERS);

            // Flush RGB values to LEDs
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, EXAMPLE_LED_NUMBERS * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(EXAMPLE_CHASE_SPEED_MS));

            // Clear LEDs
            memset(led_strip_pixels, 0, EXAMPLE_LED_NUMBERS * 3);
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, EXAMPLE_LED_NUMBERS * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(EXAMPLE_CHASE_SPEED_MS));
        }
        start_rgb += 60;
    }
}


void color_wipe_chase(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder, uint32_t color_r, uint32_t color_g, uint32_t color_b)
{
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    while (1) {
        for (int i = 0; i < EXAMPLE_LED_NUMBERS; i++) {
            memset(led_strip_pixels, 0, EXAMPLE_LED_NUMBERS * 3);
            led_strip_pixels[i * 3 + 0] = color_g;
            led_strip_pixels[i * 3 + 1] = color_b;
            led_strip_pixels[i * 3 + 2] = color_r;
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, EXAMPLE_LED_NUMBERS * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(50)); // adjust speed here
        }
    }
}

void set_all_leds_color(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder, uint8_t red, uint8_t green, uint8_t blue)
{
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    // Set color for all LEDs
    for (int i = 0; i < EXAMPLE_LED_NUMBERS; i++) {
        led_strip_pixels[i * 3 + 0] = green; // GBR format
        led_strip_pixels[i * 3 + 1] = blue;
        led_strip_pixels[i * 3 + 2] = red;
    }

    // Now transmit
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, EXAMPLE_LED_NUMBERS * 3, &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

void set_all_leds_color_from_hex(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder, uint32_t hex_color)
{
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    // Extract RGB values from the hex code
    uint8_t red = (hex_color >> 16) & 0xFF;  // Extract Red
    uint8_t green = (hex_color >> 8) & 0xFF; // Extract Green
    uint8_t blue = hex_color & 0xFF;         // Extract Blue

    // Set color for all LEDs
    for (int i = 0; i < EXAMPLE_LED_NUMBERS; i++) {
        led_strip_pixels[i * 3 + 0] = green; // GBR format
        led_strip_pixels[i * 3 + 1] = blue;
        led_strip_pixels[i * 3 + 2] = red;
    }

    // Transmit the color data
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, EXAMPLE_LED_NUMBERS * 3, &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Create RMT TX channel");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_creds_t creds = { 0 };
    if (read_wifi_creds_from_nvs(&creds)) {
        ESP_LOGI(TAG, "Trying saved WiFi credentials...");
        if (connect_to_wifi(&creds)) {
            ESP_LOGI(TAG, "Connected to saved WiFi.");
            // Do NOT return here — continue to LED init
        } else {
            ESP_LOGW(TAG, "Failed to connect. Switching to AP mode...");
            start_ap_mode();
            xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
        }
    } else {
        ESP_LOGW(TAG, "No credentials found. Switching to AP mode...");
        start_ap_mode();
        xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    }

    // LED setup should happen no matter how Wi-Fi is configured
    led_strip_pixels = (uint8_t *)malloc(EXAMPLE_LED_NUMBERS * 3 * sizeof(uint8_t));
    if (led_strip_pixels == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for LED pixels");
        return;
    }

    rmt_channel_handle_t led_chan = NULL;
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RMT_LED_STRIP_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    ESP_LOGI(TAG, "Install LED strip encoder");
    rmt_encoder_handle_t led_encoder = NULL;
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_LOGI(TAG, "Enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    ESP_LOGI(TAG, "Start LED effects");
    rainbow_chase(led_chan, led_encoder);
}

