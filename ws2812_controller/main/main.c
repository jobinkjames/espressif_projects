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
#include "mdns.h"

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      18

#define MDNS_HOSTNAME "esp32_device"    // Hostname of the ESP32 device
#define MDNS_INSTANCE_NAME "ESP32 TCP Server" 
#define WIFI_CONNECTED_BIT BIT0
#define PORT 3333
#define PORTt 8989
#define UDP_PORT 8787  // Port to listen for discovery
#define TCP_PORT 8988  // Port to respond via TCP
#define BUFFER_SIZE 1024

static const char *TAG = "example";

 uint8_t mac[6];
 char device_id[5];      // Full MAC as string "A1B2C3D4E5F6"
const char *device_type = "RGB_CON";  // Fixed device type
// Dynamic number of LEDs (adjust this value as needed)
static uint32_t EXAMPLE_LED_NUMBERS = 8; // You can dynamically change this value
float g_led_brightness = 1.0f; // Range: 0.0 (off) to 1.0 (full brightness)
int EXAMPLE_CHASE_SPEED_MS = 70;

static EventGroupHandle_t wifi_event_group;
// Array to store LED color values (RGB for each LED)
static uint8_t *led_strip_pixels = NULL;
typedef struct {
    char ssid[32];
    char password[64];
} wifi_creds_t;

void tcp_receive_task(void *pvParameters) {
    char rx_buffer[BUFFER_SIZE];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct sockaddr_in server_addr;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORTt);

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORTt);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
    }

    while (1) {
        ESP_LOGI(TAG, "Waiting for connection...");
        struct sockaddr_in client_addr;
        uint addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }
        ESP_LOGI(TAG, "Socket accepted");

        while (1) {
            int len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            } else {
                rx_buffer[len] = 0;  // Null-terminate
                ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);

                // You can store this to a global buffer or process here
            }
        }

        close(client_sock);
        ESP_LOGI(TAG, "Client socket closed");
    }

    close(listen_sock);
    vTaskDelete(NULL);
}


esp_err_t mdns_init_example(uint8_t *mac)
{
    esp_err_t err;

    // Initialize mDNS
    
    
char mac_str[5];  // 2 hex digits per byte * 2 bytes + 1 null terminator
snprintf(mac_str, sizeof(mac_str), "%02X%02X", mac[4], mac[5]);


    // Combine the base hostname with the last 3 digits of MAC address
    char full_hostname[64];
    snprintf(full_hostname, sizeof(full_hostname), "%s_%s", MDNS_HOSTNAME, mac_str);
    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to initialize mDNS");
        return err;
    }

    // Set the hostname (this will allow you to access the device with esp32_device.local)
    err = mdns_hostname_set(full_hostname);
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to set hostname");
        return err;
    }
 ESP_LOGI("mDNS", "Set mDNS hostname to: %s.local", full_hostname);
    // Set the mDNS instance name (used for service discovery)
    err = mdns_instance_name_set(MDNS_INSTANCE_NAME);
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to set instance name");
        return err;
    }

    // Register a TCP service on port 8989 (you can replace the port number)
    err = mdns_service_add(NULL, "_tcp", "_tcp", TCP_PORT, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to add TCP service");
        return err;
    }
    
    

    ESP_LOGI("mDNS", "mDNS initialized successfully with hostname: %s", full_hostname);

    return ESP_OK;
}

const char *get_my_ip_address()
{
    esp_netif_ip_info_t ip_info;
    static char ip_str[INET_ADDRSTRLEN];

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        inet_ntop(AF_INET, &ip_info.ip.addr, ip_str, sizeof(ip_str));
        return ip_str;
    } else if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        inet_ntop(AF_INET, &ip_info.ip.addr, ip_str, sizeof(ip_str));
        return ip_str;
    }

    return "0.0.0.0";
}

void send_tcp_identification(struct in_addr dest_ip) {
    ESP_LOGI(TAG, "Trying to connect to TCP server at IP: %s", inet_ntoa(dest_ip));

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create TCP socket: errno %d", errno);
        return;
    }

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr = dest_ip,
    };

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "TCP connect failed: errno %d", errno);
        close(sock);
        return;
    }

    // Get the current IP address
    const char *ip_str = get_my_ip_address();

    // Get the mDNS hostname (corrected usage)
    char dns_name[64];
    if (mdns_hostname_get(dns_name) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get mDNS hostname");
        strcpy(dns_name, "Unknown");
    }

    // Prepare the message including the DNS name, device ID, device type, and IP address
    char message[256];  // Adjusted message size to include DNS name
    snprintf(message, sizeof(message),
             "{\"id\":\"%s\",\"type\":\"%s\",\"ip\":\"%s\",\"dns\":\"%s\"}",
             device_id, device_type, ip_str, dns_name);

    // Send the message
    send(sock, message, strlen(message), 0);
    ESP_LOGI(TAG, "Sent device info to app: %s", message);

    close(sock);
}



void udp_listener_task(void *pvParameters) {
    char rx_buffer[128];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // ✅ Enable broadcast option on the socket
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "Failed to set SO_BROADCAST: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening for UDP discovery on port %d...", UDP_PORT);

    while (1) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            continue;
        }

        rx_buffer[len] = 0; // Null-terminate received data

        ESP_LOGI(TAG, "Received UDP packet: %s", rx_buffer);
        ESP_LOGI(TAG, "From %s:%d",
                 inet_ntoa(source_addr.sin_addr), ntohs(source_addr.sin_port));

        if (strcmp(rx_buffer, "DISCOVER_DEVICES") == 0) {
            ESP_LOGI(TAG, "Discovery request received");
            // Respond via TCP
            send_tcp_identification(source_addr.sin_addr);
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

void init_device_info() {
    uint8_t mac[6];
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);  // Use STA MAC for consistency

    snprintf(device_id, sizeof(device_id), "%02X%02X", mac[4], mac[5]);

    ESP_LOGI(TAG, "Device Type: %s", device_type);
    ESP_LOGI(TAG, "Device ID  : %s", device_id);
    
    esp_err_t result = mdns_init_example(mac);
    if (result == ESP_OK) {
        ESP_LOGI("mDNS", "mDNS service is running.");
    } else {
        ESP_LOGE("mDNS", "mDNS initialization failed.");
    }
}

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
 

    // Generate unique SSID using last 3 bytes of MAC
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "RGB_CONTROLLER_%02X%02X%02X", mac[3], mac[4], mac[5]);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 1,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    // Copy the generated SSID into config
    strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ssid);

    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP started. SSID: %s, Port: %d", ssid, PORT);
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
    
    init_device_info();
    
    wifi_creds_t creds = { 0 };
    if (read_wifi_creds_from_nvs(&creds)) {
        ESP_LOGI(TAG, "Trying saved WiFi credentials...");
        if (connect_to_wifi(&creds)) {
            ESP_LOGI(TAG, "Connected to saved WiFi.");
            // Do NOT return here — continue to LED init
            xTaskCreate(udp_listener_task, "udp_listener", 4096, NULL, 5, NULL);

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
xTaskCreate(tcp_receive_task, "tcp_receive", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Start LED effects");
    rainbow_chase(led_chan, led_encoder);
    
}

