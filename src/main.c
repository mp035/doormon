/**
 * Doormon – ESP32 FireBeetle V4.0
 *
 * Connects to WiFi, runs an HTTP server with /status and /reset.
 * Trigger input (falling edge) latches a "triggered" state; /reset clears it.
 * GPIO2 drives the onboard blue LED: on when triggered, off when reset.
 * Triggered state is stored in NVS and restored across (hot) reboots.
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
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* WiFi – change these for your network */
//#define WIFI_SSID      "Planet Express"
#define WIFI_SSID      "FuturePointFactory"
#define WIFI_PASSWORD  "Kelvinator"
#define WIFI_CONNECT_TIMEOUT_MS  (60 * 1000)  /* Retry for ~60s at startup, then reboot */

/* Trigger input: GPIO12 (no onboard peripheral on FireBeetle). */
#define TRIGGER_GPIO   GPIO_NUM_5
/* GPIO2 drives the onboard blue LED: on when triggered, off when reset. */
#define LED_GPIO       GPIO_NUM_2

#define NVS_NAMESPACE  "doormon"
#define NVS_KEY_TRIG   "triggered"

static const char *TAG = "doormon";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* True after first GOT_IP; used to reboot on disconnect (AP lost) instead of retrying. */
static bool s_wifi_ever_connected;

/* Latched trigger state. Written by ISR (true) and /reset (false); read by /status. */
static volatile bool s_triggered;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_ever_connected) {
            ESP_LOGW(TAG, "AP lost, rebooting to reconnect");
            esp_restart();
        }
        esp_wifi_connect();
        ESP_LOGW(TAG, "connect to AP failed, retrying...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_ever_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_wifi_ever_connected = false;

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

    ESP_LOGI(TAG, "wifi_init_sta done, waiting for AP (up to %d s)...",
             (int)(WIFI_CONNECT_TIMEOUT_MS / 1000));

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "no connection after %d s, rebooting", (int)(WIFI_CONNECT_TIMEOUT_MS / 1000));
        esp_restart();
    }
    ESP_LOGI(TAG, "connected to SSID:%s", WIFI_SSID);
    return true;
}

static void IRAM_ATTR trigger_isr_handler(void *arg)
{
    (void)arg;
    s_triggered = true;
    gpio_set_level(LED_GPIO, 1);
}

/* Load / save triggered state to NVS so it survives (hot) reboots. */
static void triggered_nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_TRIG, &v) == ESP_OK && v) {
        s_triggered = true;
        ESP_LOGI(TAG, "restored triggered state from NVS");
    }
    nvs_close(h);
}

static esp_err_t triggered_nvs_save(bool triggered)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_TRIG, triggered ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void nvs_sync_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_triggered) {
            (void)triggered_nvs_save(true);
        }
    }
}

static void trigger_gpio_init(void)
{
    /* Trigger input: falling edge latches triggered state. */
    gpio_config_t trigger_io = {
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&trigger_io));

    /* LED output (GPIO2 = onboard blue LED): on when triggered, off when reset. */
    gpio_config_t led_io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_io));
    gpio_set_level(LED_GPIO, s_triggered ? 1 : 0);

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(TRIGGER_GPIO, trigger_isr_handler, NULL));
    ESP_LOGI(TAG, "trigger GPIO %d, LED GPIO %d (falling-edge latch, LED = triggered)",
             TRIGGER_GPIO, LED_GPIO);
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
    gpio_set_level(LED_GPIO, 0);
    (void)triggered_nvs_save(false);
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

    triggered_nvs_load();   /* restore triggered state across reboots */

    (void)wifi_init_sta();  /* returns only when connected; else reboots after timeout */

    trigger_gpio_init();     /* configures GPIOs and sets LED from s_triggered */
    start_httpd();

    xTaskCreate(nvs_sync_task, "nvs_sync", 2048, NULL, 1, NULL);
}
