/**
 * Doormon – ESP32 FireBeetle V4.0
 *
 * Connects to WiFi, runs an HTTP server with /status and /reset.
 * IO2 falling edge latches a "triggered" state; /reset clears it.
 *
 * Configure WiFi below before building.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* WiFi – change these for your network */
#define WIFI_SSID      "Planet Express"
#define WIFI_PASSWORD  "Kelvinator"
#define WIFI_MAX_RETRY 5

/* IO2 on FireBeetle ESP32 (GPIO 2) */
#define TRIGGER_GPIO   GPIO_NUM_2

static const char *TAG = "doormon";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num;

/* Latched trigger state. Written by ISR (true) and /reset (false); read by /status. */
static volatile bool s_triggered;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry connect to AP (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "connect to AP failed");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta done, waiting for AP...");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "failed to connect to SSID:%s", WIFI_SSID);
        return false;
    }
    ESP_LOGI(TAG, "connected to SSID:%s", WIFI_SSID);
    return true;
}

static void IRAM_ATTR trigger_isr_handler(void *arg)
{
    (void)arg;
    s_triggered = true;
}

static void trigger_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(TRIGGER_GPIO, trigger_isr_handler, NULL));
    ESP_LOGI(TAG, "IO2 (GPIO %d) falling-edge trigger configured", TRIGGER_GPIO);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    bool t = s_triggered;
    char body[32];
    snprintf(body, sizeof(body), "{\"triggered\":%s}", t ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, strlen(body));
    return ESP_OK;
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    (void)req;
    s_triggered = false;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"reset\":true}");
    return ESP_OK;
}

static esp_err_t reset_get_handler(httpd_req_t *req)
{
    return reset_post_handler(req);
}

static httpd_handle_t start_httpd(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t status = {
        .uri    = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_register_uri_handler(server, &status);

    httpd_uri_t reset_get = {
        .uri     = "/reset",
        .method  = HTTP_GET,
        .handler = reset_get_handler,
    };
    httpd_register_uri_handler(server, &reset_get);

    httpd_uri_t reset_post = {
        .uri     = "/reset",
        .method  = HTTP_POST,
        .handler = reset_post_handler,
    };
    httpd_register_uri_handler(server, &reset_post);

    ESP_LOGI(TAG, "HTTP server started, /status and /reset");
    return server;
}

void app_main(void)
{
    s_triggered = false;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (!wifi_init_sta()) {
        ESP_LOGE(TAG, "WiFi failed, aborting");
        return;
    }

    trigger_gpio_init();
    start_httpd();
}
