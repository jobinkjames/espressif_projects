#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define WIFI_CONNECTED_BIT BIT0
#define PORT 3333

static const char *TAG = "WIFI_PROV";
static EventGroupHandle_t wifi_event_group;

typedef struct {
    char ssid[32];
    char password[64];
} wifi_creds_t;

// NVS Save
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

// Main App
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_creds_t creds = { 0 };
    if (read_wifi_creds_from_nvs(&creds)) {
        ESP_LOGI(TAG, "Trying saved WiFi credentials...");
        if (connect_to_wifi(&creds)) {
            ESP_LOGI(TAG, "Connected to saved WiFi.");
            return;
        }
    }

    ESP_LOGW(TAG, "Switching to AP mode...");
    start_ap_mode();
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}
