#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "SHOOTING_CAM";

#define WIFI_SSID       "ShootingCam_AP"
#define WIFI_PASSWORD   "12345678"
#define WIFI_MAX_CONN   4


// ============================================================
// Wi-Fi event handler
// ============================================================

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "Wi-Fi AP started");
            ESP_LOGI(TAG, "SSID: %s", WIFI_SSID);
            ESP_LOGI(TAG, "IP address: 192.168.4.1");
            break;

        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t *event =
                (wifi_event_ap_staconnected_t *)event_data;

            ESP_LOGI(
                TAG,
                "Station connected - MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                event->mac[0],
                event->mac[1],
                event->mac[2],
                event->mac[3],
                event->mac[4],
                event->mac[5]
            );

            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t *event =
                (wifi_event_ap_stadisconnected_t *)event_data;

            ESP_LOGI(
                TAG,
                "Station disconnected - MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                event->mac[0],
                event->mac[1],
                event->mac[2],
                event->mac[3],
                event->mac[4],
                event->mac[5]
            );

            break;
        }

        default:
            break;
    }
}


// ============================================================
// Wi-Fi SoftAP initialization
// ============================================================

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = sizeof(WIFI_SSID) - 1,
            .channel = 1,
            .password = WIFI_PASSWORD,
            .max_connection = WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_AP)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(TAG, "Wi-Fi AP initialization completed");
}


// ============================================================
// GET /
// ============================================================

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char *response =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Shooting Camera</title>"
        "</head>"
        "<body>"
        "<h1>ESP32-S3 Shooting Camera</h1>"
        "<p>HTTP server is running.</p>"
        "<p><a href=\"/status\">Device Status</a></p>"
        "<p><a href=\"/capture\">Capture Test</a></p>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");

    return httpd_resp_send(
        req,
        response,
        HTTPD_RESP_USE_STRLEN
    );
}


// ============================================================
// GET /status
// ============================================================

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const char *response =
        "{"
        "\"device\":\"ESP32-S3\","
        "\"project\":\"Shooting Target Camera\","
        "\"wifi\":\"AP\","
        "\"ip\":\"192.168.4.1\","
        "\"camera\":\"not_initialized\""
        "}";

    httpd_resp_set_type(req, "application/json");

    return httpd_resp_send(
        req,
        response,
        HTTPD_RESP_USE_STRLEN
    );
}


// ============================================================
// GET /capture
//
// Temporary implementation.
// Camera will be added in Phase 2.
// ============================================================

static esp_err_t capture_get_handler(httpd_req_t *req)
{
    const char *response =
        "{"
        "\"success\":false,"
        "\"message\":\"Camera not initialized\","
        "\"next_phase\":\"OV5640 initialization\""
        "}";

    httpd_resp_set_type(req, "application/json");

    return httpd_resp_send(
        req,
        response,
        HTTPD_RESP_USE_STRLEN
    );
}


// ============================================================
// Start HTTP server
// ============================================================

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server...");

    esp_err_t ret = httpd_start(
        &server,
        &config
    );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to start HTTP server: %s",
            esp_err_to_name(ret)
        );

        return NULL;
    }


    // --------------------------------------------------------
    // /
    // --------------------------------------------------------

    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(
        httpd_register_uri_handler(
            server,
            &root_uri
        )
    );


    // --------------------------------------------------------
    // /status
    // --------------------------------------------------------

    httpd_uri_t status_uri = {
        .uri      = "/status",
        .method   = HTTP_GET,
        .handler  = status_get_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(
        httpd_register_uri_handler(
            server,
            &status_uri
        )
    );


    // --------------------------------------------------------
    // /capture
    // --------------------------------------------------------

    httpd_uri_t capture_uri = {
        .uri      = "/capture",
        .method   = HTTP_GET,
        .handler  = capture_get_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(
        httpd_register_uri_handler(
            server,
            &capture_uri
        )
    );


    ESP_LOGI(
        TAG,
        "HTTP server started"
    );

    ESP_LOGI(
        TAG,
        "GET  /"
    );

    ESP_LOGI(
        TAG,
        "GET  /status"
    );

    ESP_LOGI(
        TAG,
        "GET  /capture"
    );

    return server;
}

// ============================================================
// Application entry point
// ============================================================

void app_main(void)
{
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "     Shooting Target Camera");
    ESP_LOGI(TAG, "     ESP32-S3 + HTTP Server");
    ESP_LOGI(TAG, "====================================");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());

        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Start Wi-Fi AP
    wifi_init_softap();

    // Start HTTP server
    start_webserver();

    // Keep application running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}