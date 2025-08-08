#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_private/panic_internal.h"
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
#include <inttypes.h>
#include <math.h>
#include "driver/gpio.h"
#include "freertos/semphr.h"






#define WIFI_CONNECTED_BIT BIT0
#define WIFI_STARTED_BIT   BIT1

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      18
#define LED_GPIO_NUM 2 // Default LED GPIO pin (adjust as needed)
#define MDNS_HOSTNAME "esp32_device"    // Hostname of the ESP32 device
#define MDNS_INSTANCE_NAME "ESP32 TCP Server" 
#define WIFI_CONNECTED_BIT BIT0
#define PORT 3333
#define PORTt 8989
#define UDP_PORT 8787  // Port to listen for discovery
#define TCP_PORT 8988
#define AP_LIST_PORT 8986  // Port to respond via TCP
#define BUFFER_SIZE 1024
static void start_ap_mode();
static void tcp_server_task(void *pvParameter);
char last_client_ip[INET_ADDRSTRLEN] = {0};
//int client_sock = -1;  // Store the connected client socket
int status;
char mdns_flag;
#define TCP_SERVER_TASK1_STACK_SIZE 8192 // Increased from 4096 to 8192

#define MAX_AP_NUM 20  // Limit for APs stored

wifi_ap_record_t scanned_ap_list[MAX_AP_NUM];
uint16_t scanned_ap_count = 0;
SemaphoreHandle_t ap_list_mutex;  // Mutex to protect access
SemaphoreHandle_t wifi_scan_mutex = NULL;
void wifi_scan_task(void *pvParameters);


// Move recv_buffer to static to reduce stack usage
static char recv_buffer[BUFFER_SIZE];



 static TaskHandle_t udp_listener_task_handle = NULL;
  static TaskHandle_t tcp_server_task1_handle = NULL;
  static TaskHandle_t send_ap_list_task_handle = NULL;
  static TaskHandle_t tcp_server_task_handle = NULL;
  static TaskHandle_t wifi_scan_task_handle = NULL;


static const char *TAG = "example";
const char *device_type = "RGB_CON";

 uint8_t mac[6];
 char device_id[5];      // Full MAC as string "A1B2C3D4E5F6"
static esp_netif_t *sta_netif = NULL;

typedef struct {
    rmt_channel_handle_t led_chan;
    rmt_encoder_handle_t led_encoder;
} led_task_params_t;



typedef struct {
    uint32_t led_numbers;       // Number of LEDs, can be updated dynamically
    float led_brightness;       // Brightness of LEDs
    int chase_speed_ms;
    int power;
    int mode;
    uint32_t colour;         // Speed of chase effect
} led_config_t;

// Initialize with default values
led_config_t g_led_config = {
    .led_numbers = 8,           // Default number of LEDs
    .led_brightness = 1.0f,     // Default brightness (full brightness)
    .chase_speed_ms = 70 ,
    .power = 1 ,
    .mode = 1,
    .colour = 0xffffff     
};


static EventGroupHandle_t wifi_event_group;
// Array to store LED color values (RGB for each LED)
static uint8_t *led_strip_pixels = NULL;
typedef struct {
    char ssid[32];
    char password[64];
} wifi_creds_t;
// Function to parse the TCP message and update the global buffer

void update_led_buffer(uint32_t new_led_numbers) {
    // Check if the number of LEDs has changed
    if (new_led_numbers != g_led_config.led_numbers) {
        // Reallocate memory for the LED buffer based on the new number of LEDs
        led_strip_pixels = (uint8_t *)realloc(led_strip_pixels, new_led_numbers * 3 * sizeof(uint8_t));
        if (led_strip_pixels == NULL) {
            ESP_LOGE(TAG, "Failed to reallocate memory for LED pixels");
            return;
        }
        // Update the global led_numbers to the new value
        //led_numbers = new_led_numbers;
        //ESP_LOGI(TAG, "LED buffer resized to accommodate %u LEDs", led_numbers);
    }
}

esp_err_t save_led_config_to_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("led_config", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err |= nvs_set_u32(my_handle, "led_numbers", g_led_config.led_numbers);
    err |= nvs_set_blob(my_handle, "led_brightness", &g_led_config.led_brightness, sizeof(g_led_config.led_brightness));
    err |= nvs_set_i32(my_handle, "chase_speed_ms", g_led_config.chase_speed_ms);
    err |= nvs_set_i32(my_handle, "direction", g_led_config.power);
    err |= nvs_set_i32(my_handle, "mode", g_led_config.mode);
    err |= nvs_set_u32(my_handle, "colour", g_led_config.colour);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing LED config to NVS: %s", esp_err_to_name(err));
        nvs_close(my_handle);
        return err;
    }

    err = nvs_commit(my_handle);
    nvs_close(my_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit LED config: %s", esp_err_to_name(err));
    }

    return err;
}


esp_err_t load_led_config_from_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("led_config", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Load led_numbers
    err = nvs_get_u32(my_handle, "led_numbers", &g_led_config.led_numbers);
    if (err != ESP_OK || g_led_config.led_numbers == 0) {
        ESP_LOGW(TAG, "Invalid or missing led_numbers, using default 30");
        g_led_config.led_numbers = 30;
        nvs_set_u32(my_handle, "led_numbers", g_led_config.led_numbers);
    }

    // Load led_brightness
    size_t size = sizeof(g_led_config.led_brightness);
    err = nvs_get_blob(my_handle, "led_brightness", &g_led_config.led_brightness, &size);
    if (err != ESP_OK) {
        g_led_config.led_brightness = 1.0f;
        nvs_set_blob(my_handle, "led_brightness", &g_led_config.led_brightness, size);
    }

    // Load integers with proper APIs
    err = nvs_get_i32(my_handle, "chase_speed_ms", &g_led_config.chase_speed_ms);
    if (err != ESP_OK) {
        g_led_config.chase_speed_ms = 500;
        nvs_set_i32(my_handle, "chase_speed_ms", g_led_config.chase_speed_ms);
    }

    err = nvs_get_i32(my_handle, "direction", &g_led_config.power);
    if (err != ESP_OK) {
        g_led_config.power = 0;
        nvs_set_i32(my_handle, "direction", g_led_config.power);
    }

    err = nvs_get_i32(my_handle, "mode", &g_led_config.mode);
    if (err != ESP_OK) {
        g_led_config.mode = 1;
        nvs_set_i32(my_handle, "mode", g_led_config.mode);
    }

    err = nvs_get_u32(my_handle, "colour", &g_led_config.colour);
    if (err != ESP_OK) {
        g_led_config.colour = 0xFFFFFF;
        nvs_set_u32(my_handle, "colour", g_led_config.colour);
    }

    err = nvs_commit(my_handle);
    nvs_close(my_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit changes: %s", esp_err_to_name(err));
        return err;
    }

    // Print loaded config
    printf("Updated buffer: LED Numbers: %lu, LED Brightness: %.2f, Speed: %d ms\n", 
           g_led_config.led_numbers, g_led_config.led_brightness, g_led_config.chase_speed_ms);
    printf("Direction: %d, Mode: %d, Colour: 0x%08lx\n",
           g_led_config.power,
           g_led_config.mode,
           (unsigned long)g_led_config.colour);

    return ESP_OK;
}



void update_led_config_from_message(const char* message) {
    uint32_t led_numbers;
    float led_brightness;
    int chase_speed_ms;
    int direction1;
    int mode1;
    uint32_t colour1;
    

    // Parse the message
    int parsed_items1 = sscanf(message, 
                              "colour=%08lx,brightness=%f,speed_ms=%d,effect=%u,power=%u,leds=%lu,status=%u", 
                              &colour1, &led_brightness, &chase_speed_ms, &mode1, &direction1, &led_numbers, &status);
    if (parsed_items1 == 7) {
        // Update the values
        g_led_config.led_brightness = led_brightness;
        g_led_config.chase_speed_ms = chase_speed_ms;
        g_led_config.power = direction1;
        g_led_config.mode = mode1;
        g_led_config.colour = colour1;
        g_led_config.led_numbers = led_numbers;
        update_led_buffer(led_numbers);

        // Save to NVS
        save_led_config_to_nvs();

        // Log the updated values
        ESP_LOGI(TAG, "Updated LED settings: led_numbers=%lu, led_brightness=%f, chase_speed_ms=%d",
                 led_numbers, led_brightness, chase_speed_ms);
        printf("Updated buffer: LED Numbers: %lu, LED Brightness: %.2f, Speed: %d ms\n", 
               g_led_config.led_numbers, g_led_config.led_brightness, g_led_config.chase_speed_ms);
        printf("direction: %u, mode: %u, colour: 0x%08lx, status: %d\n",
               g_led_config.power, g_led_config.mode, (unsigned long)g_led_config.colour, status);

        if (status == 1) {
            // Clean up existing tasks before switching to AP mode
            

            // Stop WiFi and disconnect STA
           

            //start_ap_mode();
            if (wifi_scan_task_handle == NULL) {

    xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, &wifi_scan_task_handle);
    ESP_LOGI(TAG, "Wi-Fi scan task created");
}
        }
    } else {
        ESP_LOGE(TAG, "Failed to parse message properly, parsed %d items", parsed_items1);
    }
}

// TCP server task
void tcp_server_task1(void *pvParameters) {
		if (tcp_server_task1_handle != NULL && tcp_server_task1_handle != xTaskGetCurrentTaskHandle()) {
    ESP_LOGW(TAG, "tcp_server_task1 is already running!");
    vTaskDelete(NULL); // kill duplicate task
}

tcp_server_task1_handle = xTaskGetCurrentTaskHandle(); // 👈 add this line at start of task

    struct sockaddr_in server_addr, client_addr;
    int sockfd, new_sock;
    socklen_t addr_len = sizeof(client_addr);
    char recv_buffer[1024];

    while (1) {
        // Get the appropriate network interface (STA or AP)
        esp_netif_t *netif = NULL;
        esp_netif_ip_info_t ip_info;
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            netif = sta_netif;
            ESP_LOGI(TAG, "TCP server binding to STA interface");
        } else if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            netif = ap_netif;
            ESP_LOGI(TAG, "TCP server binding to AP interface");
        } else {
            ESP_LOGE(TAG, "No valid network interface found. Retrying in 5 seconds...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Create socket
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Set socket options to reuse address
        int opt = 1;
setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        // Set up server address
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = ip_info.ip.addr; // Bind to the interface's IP
        server_addr.sin_port = htons(PORTt);

        // Bind socket
        if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            ESP_LOGE(TAG, "Bind failed: errno %d", errno);
            close(sockfd);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Listen for incoming connections
        if (listen(sockfd, 5) < 0) {
            ESP_LOGE(TAG, "Listen failed: errno %d", errno);
            close(sockfd);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "Server listening on %s:%d", inet_ntoa(ip_info.ip), PORTt);

        while (1) {
            // Accept a client connection
            new_sock = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);
            if (new_sock < 0) {
                ESP_LOGE(TAG, "Accept failed: errno %d", errno);
                break; // Break inner loop to recheck network interface
            }

           // Save the client IP address to last_client_ip
            //inet_ntop(AF_INET, &(client_addr.sin_addr), last_client_ip, INET_ADDRSTRLEN);
            ESP_LOGI(TAG, "Client connected from %s:%d, saved to last_client_ip",
                     last_client_ip, ntohs(client_addr.sin_port));
            // Receive the message from the client
            int recv_len = recv(new_sock, recv_buffer, sizeof(recv_buffer) - 1, 0);
            if (recv_len > 0) {
                recv_buffer[recv_len] = '\0'; // Null-terminate the received string
                ESP_LOGI(TAG, "Received message: %s", recv_buffer);
                // Update the global buffer with the received data
                update_led_config_from_message(recv_buffer);
            } else {
                ESP_LOGW(TAG, "Failed to receive data");
            }

            // Close the client connection
            close(new_sock);
        }

        // Close server socket and retry
        close(sockfd);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Small delay before restarting
    }
}



esp_err_t mdns_init_example(uint8_t *mac)
{
    esp_err_t err;
    char mac_str[5]; // 2 hex digits per byte * 2 bytes + 1 null terminator
    snprintf(mac_str, sizeof(mac_str), "%02X%02X", mac[4], mac[5]);

    char full_hostname[64];
    snprintf(full_hostname, sizeof(full_hostname), "%s_%s", MDNS_HOSTNAME, mac_str);
    
    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to initialize mDNS: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(full_hostname);
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to set hostname: %s", esp_err_to_name(err));
        mdns_free();
        return err;
    }
    ESP_LOGI("mDNS", "Set mDNS hostname to: %s.local", full_hostname);

    err = mdns_instance_name_set(MDNS_INSTANCE_NAME);
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to set instance name: %s", esp_err_to_name(err));
        mdns_free();
        return err;
    }

    // Register TCP service for LED control on port 8989
    err = mdns_service_add(NULL, "_rgbcon", "_tcp", PORTt, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to add TCP service for port %d: %s", PORTt, esp_err_to_name(err));
        mdns_free();
        return err;
    }

    // Register TCP service for discovery response on port 8988
    err = mdns_service_add(NULL, "_discovery", "_tcp", TCP_PORT, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE("mDNS", "Failed to add TCP service for port %d: %s", TCP_PORT, esp_err_to_name(err));
        mdns_free();
        return err;
    }

    ESP_LOGI("mDNS", "mDNS initialized successfully with hostname: %s.local", full_hostname);
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
	if (udp_listener_task_handle != NULL && udp_listener_task_handle != xTaskGetCurrentTaskHandle()) {
    ESP_LOGW(TAG, "tcp_server_task1 is already running!");
    vTaskDelete(NULL); // kill duplicate task
}
	udp_listener_task_handle = xTaskGetCurrentTaskHandle();
    char rx_buffer[128];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    while (1) {
        // Create UDP socket
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(5000)); // Retry after 5 seconds
            continue;
        }

        // Enable broadcast option on the socket
        int broadcast = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
            ESP_LOGE(TAG, "Failed to set SO_BROADCAST: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Set socket options to reuse address
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Set up listening address
        struct sockaddr_in listen_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(UDP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY), // Bind to all interfaces
        };

        // Bind socket
        if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
            ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Log the active mode and IP for clarity
        esp_netif_ip_info_t ip_info;
        const char *ip_str = "0.0.0.0";
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            ip_str = inet_ntoa(ip_info.ip);
            ESP_LOGI(TAG, "Listening for UDP discovery in STA mode on %s:%d", ip_str, UDP_PORT);
        } else if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            ip_str = inet_ntoa(ip_info.ip);
            ESP_LOGI(TAG, "Listening for UDP discovery in AP mode on %s:%d", ip_str, UDP_PORT);
        } else {
            ESP_LOGW(TAG, "No valid network interface found, listening on 0.0.0.0:%d", UDP_PORT);
        }

        while (1) {
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break; // Break inner loop to recreate socket
            }

            rx_buffer[len] = 0; // Null-terminate received data
            ESP_LOGI(TAG, "Received UDP packet: %s from %s:%d",
                     rx_buffer, inet_ntoa(source_addr.sin_addr), ntohs(source_addr.sin_port));

            if (strcmp(rx_buffer, "DISCOVER_DEVICES") == 0) {
                ESP_LOGI(TAG, "Discovery request received");
                // Respond via TCP
                send_tcp_identification(source_addr.sin_addr);
            }
        }

        // Close socket and retry
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Small delay before restarting
    }
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
        pixels[i] = (uint8_t)(pixels[i] * g_led_config.led_brightness);
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
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START received");
        xEventGroupSetBits(wifi_event_group, WIFI_STARTED_BIT);  // ✅ Set this!
        // esp_wifi_connect();  <-- You can optionally remove this, since reconnect_sta_mode calls it manually
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
static bool connect_to_wifi() {
    ESP_LOGI(TAG, "Connecting to WiFi...");

    // Attempt to connect
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    return bits & WIFI_CONNECTED_BIT;
}





int reconnect_sta_mode(const wifi_creds_t *creds) {
    if (!xSemaphoreTake(wifi_scan_mutex, pdMS_TO_TICKS(3000))) {
        ESP_LOGW(TAG, "Could not acquire wifi_scan_mutex, skipping STA connection");
        if (wifi_scan_task_handle == NULL) {
            xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, &wifi_scan_task_handle);
            ESP_LOGI(TAG, "Wi-Fi scan task created");
        }
        return 0;
    }

    ESP_LOGI(TAG, "Stopping AP mode...");

    // Disconnect if connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "STA connected, disconnecting...");
        esp_wifi_disconnect();
    } else {
        ESP_LOGW(TAG, "No STA connection found, skipping disconnect");
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Clean up old event group
    if (wifi_event_group) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }

    // Create new event group and re-register handlers
    wifi_event_group = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    ESP_LOGI(TAG, "Switching to STA mode...");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, creds->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, creds->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for STA_START before calling esp_wifi_connect()
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_STARTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    if (!(bits & WIFI_STARTED_BIT)) {
        ESP_LOGE(TAG, "STA_START not received, WiFi driver not ready.");
        esp_wifi_stop();
        xSemaphoreGive(wifi_scan_mutex);
        return 0;
    }

    ESP_LOGI(TAG, "Calling esp_wifi_connect...");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        xSemaphoreGive(wifi_scan_mutex);
        return 0;
    }

    ESP_LOGI(TAG, "Waiting for connection success...");
    bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi successfully.");
        if (mdns_flag) {
            init_device_info();
            mdns_flag = 0;
        }
        xSemaphoreGive(wifi_scan_mutex);
        return 1;
    } else {
        ESP_LOGE(TAG, "Failed to connect with new credentials.");
        esp_wifi_stop();
        if (wifi_scan_task_handle == NULL) {
            xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, &wifi_scan_task_handle);
            ESP_LOGI(TAG, "Wi-Fi scan task created");
        }
        xSemaphoreGive(wifi_scan_mutex);
        return 0;
    }
}



void send_ap_list_task(void *pvParameters) {
    if (send_ap_list_task_handle != NULL && send_ap_list_task_handle != xTaskGetCurrentTaskHandle()) {
        ESP_LOGW(TAG, "send_ap_list_task is already running!");
        vTaskDelete(NULL);
    }
    send_ap_list_task_handle = xTaskGetCurrentTaskHandle();

    const TickType_t delay = pdMS_TO_TICKS(5000);
    static char msg[1024];
    int sock = -1, client_sock = -1;
    struct sockaddr_in server_addr, client_addr;

    while (1) {
        ESP_LOGI(TAG, "send_ap_list_task status=%d", status);
        if (status != 1) {
            vTaskDelay(delay);
            continue;
        }

        // Create socket
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(delay);
            continue;
        }

        // Set SO_REUSEADDR to allow port reuse
        int opt = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            ESP_LOGE(TAG, "Failed to set SO_REUSEADDR: errno %d", errno);
            close(sock);
            vTaskDelay(delay);
            continue;
        }

        // Configure server address
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr("192.168.4.1");
        server_addr.sin_port = htons(AP_LIST_PORT); // 8986

        // Bind with retries
        int max_bind_retries = 3;
        bool bind_success = false;
        for (int i = 0; i < max_bind_retries && !bind_success; i++) {
            ESP_LOGI(TAG, "TCP server binding to 192.168.4.1:%d (attempt %d/%d)", AP_LIST_PORT, i + 1, max_bind_retries);
            if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
                bind_success = true;
            } else {
                ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        if (!bind_success) {
            close(sock);
            vTaskDelay(delay);
            continue;
        }

        // Listen for connections
        ESP_LOGI(TAG, "Server listening on 192.168.4.1:%d", AP_LIST_PORT);
        if (listen(sock, 1) != 0) {
            ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
            close(sock);
            vTaskDelay(delay);
            continue;
        }

        // Accept client
        socklen_t client_len = sizeof(client_addr);
        ESP_LOGI(TAG, "Waiting for client...");
        client_sock = accept(sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "Socket accept failed: errno %d", errno);
            close(sock);
            vTaskDelay(delay);
            continue;
        }

        char client_ip[16];
        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "Client connected from %s:%d", client_ip, ntohs(client_addr.sin_port));

        // Format and send AP list
        if (xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(1000))) {
            if (scanned_ap_count == 0) {
                ESP_LOGW(TAG, "No APs to send");
                snprintf(msg, sizeof(msg), "Scan Result (0 APs):\n");
            } else {
                int offset = snprintf(msg, sizeof(msg), "Scan Result (%d APs):\n", scanned_ap_count);
                for (int i = 0; i < scanned_ap_count && offset < sizeof(msg) - 64; i++) {
                    offset += snprintf(msg + offset, sizeof(msg) - offset,
                                       "%d. %s (RSSI: %d)\n", i + 1, scanned_ap_list[i].ssid, scanned_ap_list[i].rssi);
                }
            }
            xSemaphoreGive(ap_list_mutex);

            int sent = send(client_sock, msg, strlen(msg), 0);
            if (sent < 0) {
                ESP_LOGE(TAG, "Send failed: errno %d", errno);
            } else {
                ESP_LOGI(TAG, "Sent AP list:\n%s", msg);
            }
        } else {
            ESP_LOGW(TAG, "Couldn't lock scan list");
        }

        // Close sockets
        shutdown(client_sock, SHUT_RDWR);
        close(client_sock);
        shutdown(sock, SHUT_RDWR);
        close(sock);
        vTaskDelay(delay);
    }
}



void wifi_scan_task(void *pvParameters) {
    if (wifi_scan_task_handle != NULL && wifi_scan_task_handle != xTaskGetCurrentTaskHandle()) {
        ESP_LOGW(TAG, "wifi_scan_task is already running!");
        vTaskDelete(NULL);
    }
    wifi_scan_task_handle = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "Starting Wi-Fi scan task...");

    if (!xSemaphoreTake(wifi_scan_mutex, pdMS_TO_TICKS(3000))) {
        ESP_LOGW(TAG, "Could not acquire wifi_scan_mutex, skipping scan");
        goto cleanup;
    }

    // Disconnect and stop WiFi
    ESP_LOGI(TAG, "Disconnecting from current STA...");
   wifi_ap_record_t ap_info;
esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
if (err == ESP_OK) {
    ESP_LOGI(TAG, "STA connected, disconnecting...");
    ESP_ERROR_CHECK(esp_wifi_disconnect());
} else {
    ESP_LOGW(TAG, "No STA connection found, skipping disconnect");
}

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Stopping WiFi...");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    vTaskDelay(pdMS_TO_TICKS(1000));
    

    ESP_LOGI(TAG, "Switching to STA mode for scanning...");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
     wifi_config_t empty_config = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &empty_config));
    
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(1000));

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    bool scan_success = false;
    for (int i = 0; i < 3 && !scan_success; i++) {
        ESP_LOGI(TAG, "Starting Wi-Fi scan (attempt %d/3)...", i + 1);
        if (esp_wifi_scan_start(&scan_config, true) == ESP_OK) {
            scan_success = true;
        } else {
            ESP_LOGW(TAG, "Scan attempt %d failed", i + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!scan_success) {
        ESP_LOGE(TAG, "All scan attempts failed");
        goto release_mutex;
    }

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        ESP_LOGW(TAG, "No APs found");
        goto release_mutex;
    }

    wifi_ap_record_t *temp_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!temp_list) {
        ESP_LOGE(TAG, "Memory allocation failed");
        goto release_mutex;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, temp_list) == ESP_OK) {
        if (xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(1000))) {
            scanned_ap_count = (ap_count > MAX_AP_NUM) ? MAX_AP_NUM : ap_count;
            memcpy(scanned_ap_list, temp_list, scanned_ap_count * sizeof(wifi_ap_record_t));
            xSemaphoreGive(ap_list_mutex);
            ESP_LOGI(TAG, "Stored %d scanned APs", scanned_ap_count);
        } else {
            ESP_LOGW(TAG, "Failed to lock AP list mutex");
        }
    } else {
        ESP_LOGE(TAG, "Failed to retrieve scan results");
    }

    free(temp_list);

release_mutex:
    xSemaphoreGive(wifi_scan_mutex);

    // ---- Revert to AP mode ---- //
    ESP_LOGI(TAG, "Reverting to AP mode...");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));

    // Generate unique SSID
    const char *base_ssid = "RGB_CONTROLLER";
    char new_ssid[MAX_SSID_LEN];
    int duplicate_count = 0;

    if (xSemaphoreTake(ap_list_mutex, pdMS_TO_TICKS(1000))) {
        for (int i = 0; i < scanned_ap_count; i++) {
            if (strncmp((char *)scanned_ap_list[i].ssid, base_ssid, strlen(base_ssid)) == 0) {
                duplicate_count++;
            }
        }
        xSemaphoreGive(ap_list_mutex);
    }

    if (duplicate_count > 0) {
        int suffix_len = snprintf(NULL, 0, "_%d", duplicate_count);
        int base_len = MAX_SSID_LEN - suffix_len - 1;
        if (base_len < 1) base_len = 1;
        snprintf(new_ssid, sizeof(new_ssid), "%.*s_%d", base_len, base_ssid, duplicate_count);
    } else {
        strncpy(new_ssid, base_ssid, MAX_SSID_LEN - 1);
        new_ssid[MAX_SSID_LEN - 1] = '\0';
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .max_connection = 4,
            .beacon_interval = 100,
        }
    };
    strncpy((char *)ap_config.ap.ssid, new_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ESP_LOGI(TAG, "Setting AP SSID to: %s", new_ssid);

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());

    vTaskDelay(pdMS_TO_TICKS(1000));

    // ---- Restart TCP Tasks ---- //
   

    if (send_ap_list_task_handle == NULL && status == 1 ) {
        xTaskCreate(send_ap_list_task, "send_ap_list_task", 4096, NULL, 5, &send_ap_list_task_handle);
        ESP_LOGI(TAG, "Created send_ap_list_task");
    }

    if (tcp_server_task_handle == NULL && status ==1) {
        xTaskCreate(tcp_server_task, "tcp_server_task", 4096, NULL, 5, &tcp_server_task_handle);
        ESP_LOGI(TAG, "Created tcp_server_task");
    }

    
    

cleanup:
	status = 0;
    wifi_scan_task_handle = NULL;
    vTaskDelete(NULL);
}



static void start_ap_mode() {
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "RGB_CONTROLLER_%02X%02X%02X", mac[3], mac[4], mac[5]);

    // Stop WiFi and ensure STA is disconnected
    ESP_LOGI(TAG, "Stopping WiFi to reset state...");
    esp_wifi_disconnect(); // Disconnect STA if connected
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to ensure clean state

   

    // Check if AP interface exists
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ESP_LOGE(TAG, "AP interface not initialized");
        return;
    }

    // Set WiFi mode to AP+STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 1,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for WiFi to be fully started
    vTaskDelay(pdMS_TO_TICKS(2000)); // Additional delay to ensure WiFi is ready
    ESP_LOGI(TAG, "AP+STA mode started. SSID: %s, Port: %d", ssid, PORTt);
    if (wifi_scan_task_handle == NULL) {

    xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, &wifi_scan_task_handle);
    ESP_LOGI(TAG, "Wi-Fi scan task created");
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
    
     // If the task is already running, delete it first
if (tcp_server_task1_handle != NULL) {
    ESP_LOGI(TAG, "Deleting existing tcp_server_task1...");
    vTaskDelete(tcp_server_task1_handle);
    tcp_server_task1_handle = NULL;
    vTaskDelay(pdMS_TO_TICKS(100)); // Give scheduler time to clean up
}

// Now create it again
ESP_LOGI(TAG, "Creating new tcp_server_task1...");
xTaskCreate(tcp_server_task1, "tcp_receive", 4096, NULL, 5, &tcp_server_task1_handle);

	ESP_LOGI(TAG, "Deleting send_ap_list_task_...");
        vTaskDelete(send_ap_list_task_handle);
        send_ap_list_task_handle = NULL;
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


// Effect 1: Solid Color (Optimized from set_all_leds_color_from_hex)
void solid_color(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    rmt_transmit_config_t tx_config = { .loop_count = 0 };

    uint8_t blue = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t red = g_led_config.colour & 0xFF;

    for (int i = 0; i < g_led_config.led_numbers; i++) {
        led_strip_pixels[i * 3 + 0] = green;
        led_strip_pixels[i * 3 + 1] = blue;
        led_strip_pixels[i * 3 + 2] = red;
    }

    apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

// Effect 2: Rainbow Chase (Optimized from rainbow_chase)
void rainbow_chase(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting rainbow chase effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint32_t red, green, blue;
    uint16_t start_rgb = 0;

    while (g_led_config.mode == 2) {
        for (int i = 0; i < 3; i++) {
            memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
            if (g_led_config.power == 1) { // Forward
                for (int j = i; j < g_led_config.led_numbers; j += 3) {
                    uint16_t hue = (j * 360 / g_led_config.led_numbers + start_rgb) % 360;
                    led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
                    led_strip_pixels[j * 3 + 0] = green;
                    led_strip_pixels[j * 3 + 1] = blue;
                    led_strip_pixels[j * 3 + 2] = red;
                }
            } else { // Reverse
                for (int j = g_led_config.led_numbers - 1 - i; j >= 0; j -= 3) {
                    uint16_t hue = (j * 360 / g_led_config.led_numbers + start_rgb) % 360;
                    led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
                    led_strip_pixels[j * 3 + 0] = green;
                    led_strip_pixels[j * 3 + 1] = blue;
                    led_strip_pixels[j * 3 + 2] = red;
                }
            }

            apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
        }
        start_rgb = (start_rgb + 60) % 360;
    }
}

// Effect 3: Color Wipe Chase (Optimized from color_wipe_chase)
void color_wipe_chase(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting color wipe chase effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
   

    while (g_led_config.mode == 3) {
        // Reverse
 	uint8_t blue = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t red = g_led_config.colour & 0xFF;
            for (int i = g_led_config.led_numbers - 1; i >= 0; i--) {
                memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
                led_strip_pixels[i * 3 + 0] = green;
                led_strip_pixels[i * 3 + 1] = blue;
                led_strip_pixels[i * 3 + 2] = red;
                apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
                ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
                ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
                vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
            }
        
    }
}

// Effect 4: Theater Chase (Optimized from theater_chase_effect)
void theater_chase_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting theater chase effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 4) {
        for (int j = 0; j < 3; j++) {
            memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
            for (int i = j; i < g_led_config.led_numbers; i += 3) {
                led_strip_pixels[i * 3 + 0] = green;
                led_strip_pixels[i * 3 + 1] = blue;
                led_strip_pixels[i * 3 + 2] = red;
            }
            apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
        }
    }
}

// Effect 5: Running Lights (Optimized from running_lights_effect)
void running_lights_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting running lights effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 5) {
        // Reverse
            for (int i = g_led_config.led_numbers - 1; i >= 0; i--) {
                memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
                led_strip_pixels[i * 3 + 0] = green;
                led_strip_pixels[i * 3 + 1] = blue;
                led_strip_pixels[i * 3 + 2] = red;
                apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
                ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
                ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
                vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
            }
        
    }
}

// Effect 6: Breathing (Optimized from breathing_effect)
void breathing_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting breathing effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 6) {
        for (int brightness = 0; brightness <= 255; brightness += 5) {
            for (int i = 0; i < g_led_config.led_numbers; i++) {
                led_strip_pixels[i * 3 + 0] = (green * brightness) / 255;
                led_strip_pixels[i * 3 + 1] = (blue * brightness) / 255;
                led_strip_pixels[i * 3 + 2] = (red * brightness) / 255;
            }
            apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 10));
        }
        for (int brightness = 255; brightness >= 0; brightness -= 5) {
            for (int i = 0; i < g_led_config.led_numbers; i++) {
                led_strip_pixels[i * 3 + 0] = (green * brightness) / 255;
                led_strip_pixels[i * 3 + 1] = (blue * brightness) / 255;
                led_strip_pixels[i * 3 + 2] = (red * brightness) / 255;
            }
            apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 10));
        }
    }
}

// Effect 7: Random Flash (Optimized from random_flash_effect)
void random_flash_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting random flash effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };

    while (g_led_config.mode == 7) {
        uint8_t red = rand() % 256;
        uint8_t green = rand() % 256;
        uint8_t blue = rand() % 256;
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            led_strip_pixels[i * 3 + 0] = green;
            led_strip_pixels[i * 3 + 1] = blue;
            led_strip_pixels[i * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}

// Effect 8: Gradient Fade
void gradient_fade(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting gradient fade effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint32_t red, green, blue;
    uint16_t start_hue = 0;

    while (g_led_config.mode == 8) {
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            uint16_t hue = (start_hue + (i * 360 / g_led_config.led_numbers)) % 360;
            led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
            led_strip_pixels[i * 3 + 0] = green;
            led_strip_pixels[i * 3 + 1] = blue;
            led_strip_pixels[i * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        start_hue = (start_hue + 5) % 360;
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}

// Effect 9: Sparkle
void sparkle_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting sparkle effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 9) {
        memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
        for (int i = 0; i < g_led_config.led_numbers / 10; i++) {
            int led = rand() % g_led_config.led_numbers;
            led_strip_pixels[led * 3 + 0] = green;
            led_strip_pixels[led * 3 + 1] = blue;
            led_strip_pixels[led * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 2));
    }
}

// Effect 10: Fire Simulation
void fire_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting fire effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };

    while (g_led_config.mode == 10) {
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            int intensity = rand() % 100;
            uint8_t red = intensity * 2.55;
            uint8_t green = intensity * 0.5;
            uint8_t blue = intensity * 0.1;
            led_strip_pixels[i * 3 + 0] = green;
            led_strip_pixels[i * 3 + 1] = blue;
            led_strip_pixels[i * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 5));
    }
}

// Effect 11: Water Ripple
void water_ripple_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting water ripple effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint32_t red, green, blue;
    uint16_t center = 0;

    while (g_led_config.mode == 11) {
        memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            int distance = abs(i - center);
            uint8_t intensity = 255 * exp(-distance * 0.1);
            led_strip_hsv2rgb(180, 100, intensity, &red, &green, &blue);
            led_strip_pixels[i * 3 + 0] = green;
            led_strip_pixels[i * 3 + 1] = blue;
            led_strip_pixels[i * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        center = (center + 1) % g_led_config.led_numbers;
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}

// Effect 12: Meteor Rain
void meteor_rain_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting meteor rain effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 12) {
        for (int pos = 0; pos < g_led_config.led_numbers + 5; pos++) {
            memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
            for (int i = pos; i > pos - 5 && i >= 0; i--) {
                uint8_t intensity = 255 * (1.0 - (pos - i) * 0.2);
                led_strip_pixels[i * 3 + 0] = (green * intensity) / 255;
                led_strip_pixels[i * 3 + 1] = (blue * intensity) / 255;
                led_strip_pixels[i * 3 + 2] = (red * intensity) / 255;
            }
            apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
        }
    }
}

// Effect 13: Twinkle
void twinkle_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting twinkle effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 13) {
        memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
        for (int i = 0; i < g_led_config.led_numbers / 5; i++) {
            int led = rand() % g_led_config.led_numbers;
            uint8_t intensity = rand() % 256;
            led_strip_pixels[led * 3 + 0] = (green * intensity) / 255;
            led_strip_pixels[led * 3 + 1] = (blue * intensity) / 255;
            led_strip_pixels[led * 3 + 2] = (red * intensity) / 255;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 3));
    }
}

// Effect 14: Pulse
void pulse_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting pulse effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 14) {
        for (int brightness = 0; brightness <= 255; brightness += 5) {
            for (int i = 0; i < g_led_config.led_numbers; i++) {
                led_strip_pixels[i * 3 + 0] = (green * brightness) / 255;
                led_strip_pixels[i * 3 + 1] = (blue * brightness) / 255;
                led_strip_pixels[i * 3 + 2] = (red * brightness) / 255;
            }
            apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
            ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
            ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
            vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 20));
        }
    }
}

// Effect 15: Strobe
void strobe_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting strobe effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 15) {
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            led_strip_pixels[i * 3 + 0] = green;
            led_strip_pixels[i * 3 + 1] = blue;
            led_strip_pixels[i * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 2));

        memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms / 2));
    }
}

// Effect 16: Color Wave
void color_wave_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting color wave effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint32_t red, green, blue;
    uint16_t phase = 0;

    while (g_led_config.mode == 16) {
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            uint16_t hue = (phase + (i * 360 / g_led_config.led_numbers)) % 360;
            led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
            led_strip_pixels[i * 3 + 0] = green;
            led_strip_pixels[i * 3 + 1] = blue;
            led_strip_pixels[i * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        phase = (phase + 10) % 360;
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}

// Effect 17: Bouncing Balls
void bouncing_balls_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting bouncing balls effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;
    int positions[3] = {0, g_led_config.led_numbers / 3, g_led_config.led_numbers * 2 / 3};
    int velocities[3] = {1, 1, 1};

    while (g_led_config.mode == 17) {
        memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
        for (int i = 0; i < 3; i++) {
            led_strip_pixels[positions[i] * 3 + 0] = green;
            led_strip_pixels[positions[i] * 3 + 1] = blue;
            led_strip_pixels[positions[i] * 3 + 2] = red;
            positions[i] += velocities[i];
            if (positions[i] >= g_led_config.led_numbers - 1 || positions[i] <= 0) {
                velocities[i] = -velocities[i];
            }
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}

// Effect 18: Rainbow Cycle
void rainbow_cycle_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting rainbow cycle effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint32_t red, green, blue;
    uint16_t hue = 0;

    while (g_led_config.mode == 18) {
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            led_strip_hsv2rgb(hue, 100, 100, &red, &green, &blue);
            led_strip_pixels[i * 3 + 0] = green;
            led_strip_pixels[i * 3 + 1] = blue;
            led_strip_pixels[i * 3 + 2] = red;
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        hue = (hue + 5) % 360;
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}

// Effect 19: Chase Fade
void chase_fade_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting chase fade effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint8_t red = (g_led_config.colour >> 16) & 0xFF;
    uint8_t green = (g_led_config.colour >> 8) & 0xFF;
    uint8_t blue = g_led_config.colour & 0xFF;

    while (g_led_config.mode == 19) {
        if (g_led_config.power == 1) {
            for (int pos = 0; pos < g_led_config.led_numbers + 5; pos++) {
                memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
                for (int i = pos; i > pos - 5 && i >= 0; i--) {
                    uint8_t intensity = 255 * (1.0 - (pos - i) * 0.2);
                    led_strip_pixels[i * 3 + 0] = (green * intensity) / 255;
                    led_strip_pixels[i * 3 + 1] = (blue * intensity) / 255;
                    led_strip_pixels[i * 3 + 2] = (red * intensity) / 255;
                }
                apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
                ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
                ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
                vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
            }
        } else {
            for (int pos = g_led_config.led_numbers - 1; pos >= -5; pos--) {
                memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
                for (int i = pos; i < pos + 5 && i < g_led_config.led_numbers; i++) {
                    uint8_t intensity = 255 * (1.0 - (i - pos) * 0.2);
                    led_strip_pixels[i * 3 + 0] = (green * intensity) / 255;
                    led_strip_pixels[i * 3 + 1] = (blue * intensity) / 255;
                    led_strip_pixels[i * 3 + 2] = (red * intensity) / 255;
                }
                apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
                ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
                ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
                vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
            }
        }
    }
}

// Effect 20: Color Blend
void color_blend_effect(rmt_channel_handle_t led_chan, rmt_encoder_handle_t led_encoder)
{
    ESP_LOGI(TAG, "Starting color blend effect");
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    uint32_t red1, green1, blue1, red2, green2, blue2;
    uint16_t hue1 = 0, hue2 = 180;

    while (g_led_config.mode == 20) {
        led_strip_hsv2rgb(hue1, 100, 100, &red1, &green1, &blue1);
        led_strip_hsv2rgb(hue2, 100, 100, &red2, &green2, &blue2);
        for (int i = 0; i < g_led_config.led_numbers; i++) {
            float blend = (float)i / g_led_config.led_numbers;
            led_strip_pixels[i * 3 + 0] = (uint8_t)(green1 * (1.0 - blend) + green2 * blend);
            led_strip_pixels[i * 3 + 1] = (uint8_t)(blue1 * (1.0 - blend) + blue2 * blend);
            led_strip_pixels[i * 3 + 2] = (uint8_t)(red1 * (1.0 - blend) + red2 * blend);
        }
        apply_global_brightness(led_strip_pixels, g_led_config.led_numbers);
        ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
        hue1 = (hue1 + 5) % 360;
        hue2 = (hue2 + 5) % 360;
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}

// Updated LED Effects Task
void update_led_effects(void *pvParameters)
{
    led_task_params_t *params = (led_task_params_t *)pvParameters;
    rmt_channel_handle_t led_chan = params->led_chan;
    rmt_encoder_handle_t led_encoder = params->led_encoder;

    while (true) {
        switch (g_led_config.mode) {
            case 1:  solid_color(led_chan, led_encoder); break;
            case 2:  rainbow_chase(led_chan, led_encoder); break;
            case 3:  color_wipe_chase(led_chan, led_encoder); break;
            case 4:  theater_chase_effect(led_chan, led_encoder); break;
            case 5:  running_lights_effect(led_chan, led_encoder); break;
            case 6:  breathing_effect(led_chan, led_encoder); break;
            case 7:  random_flash_effect(led_chan, led_encoder); break;
            case 8:  gradient_fade(led_chan, led_encoder); break;
            case 9:  sparkle_effect(led_chan, led_encoder); break;
            case 10: fire_effect(led_chan, led_encoder); break;
            case 11: water_ripple_effect(led_chan, led_encoder); break;
            case 12: meteor_rain_effect(led_chan, led_encoder); break;
            case 13: twinkle_effect(led_chan, led_encoder); break;
            case 14: pulse_effect(led_chan, led_encoder); break;
            case 15: strobe_effect(led_chan, led_encoder); break;
            case 16: color_wave_effect(led_chan, led_encoder); break;
            case 17: bouncing_balls_effect(led_chan, led_encoder); break;
            case 18: rainbow_cycle_effect(led_chan, led_encoder); break;
            case 19: chase_fade_effect(led_chan, led_encoder); break;
            case 20: color_blend_effect(led_chan, led_encoder); break;
            default:
                memset(led_strip_pixels, 0, g_led_config.led_numbers * 3);
                rmt_transmit_config_t tx_config = { .loop_count = 0 };
                ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_strip_pixels, g_led_config.led_numbers * 3, &tx_config));
                ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(g_led_config.chase_speed_ms));
    }
}


void app_main(void)
{
    ESP_LOGI(TAG, "Start app");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_STA, mac));

    ESP_LOGI(TAG, "Loading LED config from NVS...");
    if (load_led_config_from_nvs() != ESP_OK || g_led_config.led_numbers == 0) {
        ESP_LOGW(TAG, "LED config failed or led_numbers is zero, setting defaults");
        g_led_config.led_numbers = 8;
    }

    led_strip_pixels = (uint8_t *)malloc(g_led_config.led_numbers * 3);
    if (!led_strip_pixels) {
        ESP_LOGE(TAG, "Failed to allocate memory for LED pixels");
        return;
    }
 // Configure default LED
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO_NUM, 1));
    ESP_LOGI(TAG, "Default LED turned on in AP mode");
    rmt_channel_handle_t led_chan = NULL;
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RMT_LED_STRIP_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    rmt_encoder_handle_t led_encoder = NULL;
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    led_task_params_t params = {
        .led_chan = led_chan,
        .led_encoder = led_encoder
    };

    ap_list_mutex = xSemaphoreCreateMutex();
    wifi_scan_mutex = xSemaphoreCreateBinary();
    if (!wifi_scan_mutex) {
        ESP_LOGE(TAG, "Failed to create wifi_scan_mutex");
        return;
    }
    xSemaphoreGive(wifi_scan_mutex);

    xTaskCreate(update_led_effects, "update_led_effects", 4096, &params, 5, NULL);
	mdns_flag =1;
    wifi_creds_t creds = {0};
    if (read_wifi_creds_from_nvs(&creds)) {
        ESP_LOGI(TAG, "Trying saved WiFi credentials...");
        if (reconnect_sta_mode(&creds)) {
            ESP_LOGI(TAG, "Connected to saved WiFi.");
        } else {
            ESP_LOGW(TAG, "Failed to connect. Switching to AP mode...");
            if (!wifi_scan_task_handle) {
                xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, &wifi_scan_task_handle);
                ESP_LOGI(TAG, "Wi-Fi scan task created");
            }
        }
    } else {
        ESP_LOGW(TAG, "No credentials found. Switching to AP mode...");
        if (!wifi_scan_task_handle) {
            xTaskCreate(wifi_scan_task, "wifi_scan_task", 4096, NULL, 5, &wifi_scan_task_handle);
            ESP_LOGI(TAG, "Wi-Fi scan task created");
        }
    }

    if (!udp_listener_task_handle) {
        xTaskCreate(udp_listener_task, "udp_listener", 4096, NULL, 5, &udp_listener_task_handle);
    }
    if (!tcp_server_task1_handle) {
        xTaskCreate(tcp_server_task1, "tcp_receive", 4096, NULL, 5, &tcp_server_task1_handle);
    }

    printf("Updated buffer: LED Numbers: %lu, LED Brightness: %.2f, Speed: %d ms\n", 
           g_led_config.led_numbers, g_led_config.led_brightness, g_led_config.chase_speed_ms);
    printf("direction: %u, mode: %u, colour: 0x%08lx\n",
           g_led_config.power, g_led_config.mode, (unsigned long)g_led_config.colour);
}