// https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/get-started/index.html

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#define SWITCH_GPIO 2      // GPIO2 as input (switch, must be HIGH at boot)
#define OUTPUT_GPIO 0      // GPIO0 as output (LED, active LOW)
#define DEBOUNCE_MS 50

static const char *TAG = "ESP01S";
static QueueHandle_t gpio_evt_queue = NULL;
static volatile int output_state = 0;

#if __has_include("credentials.h")
#  include "credentials.h"
#endif

#ifndef CREDENTIALS_H
#define WIFI_SSID "default_ssid"
#define WIFI_PASS "default_pass"
#endif

// Debounce logic
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
    // Debug: ISR called
    ets_printf("ISR: GPIO %d triggered\n", gpio_num);
}

void switch_task(void* arg) {
    uint32_t io_num;
    int last_state = gpio_get_level(SWITCH_GPIO);
    while (1) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            int level = gpio_get_level(SWITCH_GPIO);
            ESP_LOGI(TAG, "Switch event: GPIO%d level=%d last_state=%d", SWITCH_GPIO, level, last_state);
            // Only toggle on falling edge (HIGH to LOW)
            if (last_state == 1 && level == 0) {
                output_state = !output_state;
                gpio_set_level(OUTPUT_GPIO, output_state);
                ESP_LOGI(TAG, "Switch toggled, output: %d", output_state);
                vTaskDelay(DEBOUNCE_MS / portTICK_PERIOD_MS); // Debounce delay
            }
            last_state = level;
        }
    }

}

// WiFi event handler for diagnostics
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected!");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: %s", ip4addr_ntoa(&event->ip_info.ip));
    }
}


esp_err_t ui_get_handler(httpd_req_t *req) {
    char resp[256];
    snprintf(resp, sizeof(resp),
        "<html><body><h2>ESP01S Output Control</h2>"
        "<p>Output is <b>%s</b></p>"
        "<form method='POST'><button name='toggle' value='1'>Toggle Output</button></form>"
        "</body></html>",
        output_state ? "ON" : "OFF");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

esp_err_t ui_post_handler(httpd_req_t *req) {
    output_state = !output_state;
    gpio_set_level(OUTPUT_GPIO, output_state);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &config);
    httpd_uri_t ui_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ui_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t ui_post = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = ui_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ui_get);
    httpd_register_uri_handler(server, &ui_post);
}

void wifi_init_sta(void) {
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_set_direction(OUTPUT_GPIO, GPIO_MODE_DEF_OUTPUT);
    gpio_set_level(OUTPUT_GPIO, output_state);
    gpio_set_direction(SWITCH_GPIO, GPIO_MODE_DEF_INPUT);
    gpio_set_pull_mode(SWITCH_GPIO, GPIO_PULLUP_ONLY);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_set_intr_type(SWITCH_GPIO, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(SWITCH_GPIO, gpio_isr_handler, (void*) SWITCH_GPIO);
    xTaskCreate(switch_task, "switch_task", 2048, NULL, 10, NULL);

    wifi_init_sta();
    start_webserver();
    ESP_LOGI(TAG, "Setup complete.");
}
