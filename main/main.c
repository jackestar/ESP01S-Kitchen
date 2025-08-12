#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/uart.h"

#define SWITCH_GPIO 2  // GPIO2 as input (must be HIGH at boot)
#define OUTPUT_GPIO 0  // GPIO0 as output for the heating element
#define ACTIVE_GPIO 3  // RX as an indicator LED
#define ACTIVE2_GPIO 1 // TX as an indicator LED

#define DEBOUNCE_MS 250
#define MINUTE 60000 // Milliseconds in a minute

// --- Global State and Configuration ---
static volatile bool system_on = false;
static volatile bool output_state = false; // Represents the physical output pin state
static volatile int initial_on_time = 1;   // minutes
static volatile int pwm_period = 0;        // minutes (0 = disabled)
static volatile int pwm_duty = 100;        // percent
static volatile int timer_minutes = 0;     // minutes (0 = disabled)
static volatile int time_left_seconds = 0; // Time left for the timer in seconds

// --- RTOS Handles ---
static QueueHandle_t gpio_evt_queue = NULL;
static SemaphoreHandle_t state_mutex = NULL; // Mutex for protecting shared variables
static TaskHandle_t heater_task_handle = NULL;
static TaskHandle_t timer_task_handle = NULL;


#if __has_include("credentials.h")
#include "credentials.h"
#endif

#ifndef CREDENTIALS_H
#define WIFI_SSID "default_ssid"
#define WIFI_PASS "default_pass"
#endif


// Forward declarations
void heater_control_task(void *arg);
void timer_task(void *arg);

/**
 * @brief A responsive delay that checks system status frequently.
 * @param ms The total duration to delay in milliseconds.
 * @return true if the delay completed fully, false if it was aborted by the system turning off.
 */
static bool responsive_delay_ms(uint32_t ms) {
    TickType_t start_ticks = xTaskGetTickCount();
    // Use a 64-bit integer to prevent overflow with long durations
    uint64_t duration_ticks = (uint64_t)ms / portTICK_PERIOD_MS;

    while ((xTaskGetTickCount() - start_ticks) < duration_ticks) {
        // Check system status frequently (e.g., every 100ms)
        vTaskDelay(pdMS_TO_TICKS(100));

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool is_on = system_on;
        xSemaphoreGive(state_mutex);

        // If system was turned off externally, abort the delay
        if (!is_on) {
            return false;
        }
    }
    return true; // Delay completed without interruption
}

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

void switch_task(void *arg) {
    uint32_t io_num;
    TickType_t last_interrupt_time = 0;

    while (1) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            TickType_t current_time = xTaskGetTickCount();
            if ((current_time - last_interrupt_time) * portTICK_PERIOD_MS > DEBOUNCE_MS) {
                // Only toggle on falling edge (button press)
                if (gpio_get_level(SWITCH_GPIO) == 0) {
                    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                        system_on = !system_on;
                        // If turning off, also cancel the timer
                        if (!system_on) {
                            timer_minutes = 0;
                            time_left_seconds = 0;
                        }
                        xSemaphoreGive(state_mutex);
                    }
                }
                last_interrupt_time = current_time;
            }
        }
    }
}

// --- Web Server ---
const char* get_status_string() {
    if (!system_on) return "Off";
    if (timer_minutes > 0 && pwm_period > 0) return "On - Timer & PWM";
    if (timer_minutes > 0) return "On - Timer";
    if (pwm_period > 0) return "On - PWM";
    return "On";
}

esp_err_t ui_get_handler(httpd_req_t *req) {
    char resp[1024];
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    snprintf(resp, sizeof(resp),
             "<html><head><title>ESP01S Heater Control</title><meta http-equiv='refresh' content='5'></head><body>"
             "<h2>ESP01S Output Control</h2>"
             "<p>System Status: <b>%s</b></p>"
             "<p>Heater Output: <b>%s</b></p>"
             "<p>Time Left: <b>%d</b> minutes <b>%d</b> seconds</p>"
             "<form method='POST'>"
             "<button name='toggle' value='1'>Toggle System</button>"
             "</form>"
             "<h3>Configuration</h3>"
             "<form method='POST'>"
             "Initial ON time (min): <input type='number' name='initial_on_time' value='%d'><br>"
             "PWM Period (min): <input type='number' name='pwm_period' value='%d'><br>"
             "PWM Duty (%%): <input type='number' name='pwm_duty' value='%d'><br>"
             "Timer (min): <input type='number' name='timer_minutes' value='%d'><br>"
             "<input type='submit' value='Update Config'>"
             "</form>"
             "</body></html>",
             get_status_string(),
             output_state ? "ON" : "OFF",
             time_left_seconds / 60, time_left_seconds % 60,
             initial_on_time, pwm_period, pwm_duty, timer_minutes);
    xSemaphoreGive(state_mutex);
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

esp_err_t ui_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        if (strstr(buf, "toggle=1")) {
            system_on = !system_on;
            if (!system_on) { // If turning off, cancel timer
                timer_minutes = 0;
                time_left_seconds = 0;
            }
        }
        char val_buf[10];
        if (httpd_query_key_value(buf, "initial_on_time", val_buf, sizeof(val_buf)) == ESP_OK) initial_on_time = atoi(val_buf);
        if (httpd_query_key_value(buf, "pwm_period", val_buf, sizeof(val_buf)) == ESP_OK) pwm_period = atoi(val_buf);
        if (httpd_query_key_value(buf, "pwm_duty", val_buf, sizeof(val_buf)) == ESP_OK) pwm_duty = atoi(val_buf);
        if (httpd_query_key_value(buf, "timer_minutes", val_buf, sizeof(val_buf)) == ESP_OK) timer_minutes = atoi(val_buf);
        xSemaphoreGive(state_mutex);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// --- Control Tasks ---

void heater_control_task(void *arg) {
    bool was_on = false;
    bool initial_period_has_run = false;

    while (1) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool is_on = system_on;

        // On state transition from OFF to ON, reset the initial run flag.
        if (is_on && !was_on) {
            initial_period_has_run = false;
        }
        was_on = is_on;

        // If system is OFF, ensure all hardware is off and wait.
        if (!is_on) {
            if (output_state) {
                gpio_set_level(OUTPUT_GPIO, 0);
                gpio_set_level(ACTIVE_GPIO, 0);
                gpio_set_level(ACTIVE2_GPIO, 0);
                output_state = false;
            }
            xSemaphoreGive(state_mutex);
            vTaskDelay(pdMS_TO_TICKS(100)); // Short delay while idle
            continue;
        }

        // --- System is ON ---
        gpio_set_level(ACTIVE_GPIO, 1);
        gpio_set_level(ACTIVE2_GPIO, 1);

        int local_initial_on_time = initial_on_time;
        int local_pwm_period = pwm_period;
        int local_pwm_duty = pwm_duty;
        xSemaphoreGive(state_mutex);


        // --- Priority 1: Initial ON Period ----
        if (local_initial_on_time > 0 && !initial_period_has_run) {
            gpio_set_level(OUTPUT_GPIO, 1);
            xSemaphoreTake(state_mutex, portMAX_DELAY); output_state = true; xSemaphoreGive(state_mutex);

            // Use the responsive delay. If it's aborted, restart the loop.
            if (!responsive_delay_ms((uint32_t)local_initial_on_time * MINUTE)) {
                continue;
            }
            initial_period_has_run = true;
            continue; // Re-evaluate state immediately after this period
        }

        // --- Priority 2: PWM or Steady-ON ----
        if (local_pwm_period > 0) {
            if (local_pwm_duty >= 100) {
                gpio_set_level(OUTPUT_GPIO, 1);
                xSemaphoreTake(state_mutex, portMAX_DELAY); output_state = true; xSemaphoreGive(state_mutex);
                if (!responsive_delay_ms(1000)) continue; // Check state every second
            } else if (local_pwm_duty <= 0) {
                gpio_set_level(OUTPUT_GPIO, 0);
                xSemaphoreTake(state_mutex, portMAX_DELAY); output_state = false; xSemaphoreGive(state_mutex);
                if (!responsive_delay_ms(1000)) continue; // Check state every second
            } else {
                uint32_t period_ms = (uint32_t)local_pwm_period * MINUTE;
                uint32_t on_time_ms = (period_ms * local_pwm_duty) / 100;

                gpio_set_level(OUTPUT_GPIO, 1);
                xSemaphoreTake(state_mutex, portMAX_DELAY); output_state = true; xSemaphoreGive(state_mutex);
                if (!responsive_delay_ms(on_time_ms)) continue;

                gpio_set_level(OUTPUT_GPIO, 0);
                xSemaphoreTake(state_mutex, portMAX_DELAY); output_state = false; xSemaphoreGive(state_mutex);
                if (!responsive_delay_ms(period_ms - on_time_ms)) continue;
            }
        } else { // No PWM, steady ON
            gpio_set_level(OUTPUT_GPIO, 1);
            xSemaphoreTake(state_mutex, portMAX_DELAY); output_state = true; xSemaphoreGive(state_mutex);
            if (!responsive_delay_ms(500)) continue; // Yield periodically
        }
    }
}

void timer_task(void *arg) {
    int local_timer_minutes = 0;
    bool timer_was_active = false;

    while (1) {
        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            local_timer_minutes = timer_minutes;

            if (system_on && local_timer_minutes > 0 && !timer_was_active) {
                // Timer was just started
                time_left_seconds = local_timer_minutes * 60;
                timer_was_active = true;
            } else if (!system_on || local_timer_minutes == 0) {
                // Reset timer if system is off or timer is set to 0
                time_left_seconds = 0;
                timer_was_active = false;
            }

            if (timer_was_active && time_left_seconds > 0) {
                time_left_seconds--;
                if (time_left_seconds <= 0) {
                    system_on = false; // Timer expired, turn system off
                    timer_minutes = 0; // Clear the timer setting
                    timer_was_active = false;
                }
            }
            xSemaphoreGive(state_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
    }
}


// --- Initialization Functions ---
void wifi_init_sta(void);
void start_webserver(void);
void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // --- FIX FOR RX/TX PINS ---
    // Detach the default UART driver from GPIO1 and GPIO3
    // This MUST be done before configuring them as outputs.
    uart_driver_delete(UART_NUM_0);

    state_mutex = xSemaphoreCreateMutex();

    gpio_set_direction(OUTPUT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(OUTPUT_GPIO, 0);
    gpio_set_direction(ACTIVE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ACTIVE_GPIO, 0);
    gpio_set_direction(ACTIVE2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ACTIVE2_GPIO, 0);
    
    gpio_set_direction(SWITCH_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SWITCH_GPIO, GPIO_PULLUP_ONLY);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_set_intr_type(SWITCH_GPIO, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(SWITCH_GPIO, gpio_isr_handler, (void *)SWITCH_GPIO);

    wifi_init_sta();
    start_webserver();

    xTaskCreate(switch_task, "switch_task", 2048, NULL, 10, NULL);
    xTaskCreate(heater_control_task, "heater_control_task", 2048, NULL, 5, &heater_task_handle);
    xTaskCreate(timer_task, "timer_task", 2048, NULL, 4, &timer_task_handle);
}

// --- Helper Functions for WiFi/Webserver (unchanged from original) ---

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    }
}

void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &config);
    httpd_uri_t ui_get = {.uri = "/", .method = HTTP_GET, .handler = ui_get_handler, .user_ctx = NULL};
    httpd_uri_t ui_post = {.uri = "/", .method = HTTP_POST, .handler = ui_post_handler, .user_ctx = NULL};
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
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, }, };
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}