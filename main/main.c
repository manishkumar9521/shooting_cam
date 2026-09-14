#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_camera.h"
#include <stdbool.h>
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
// MJPEG stream configuration
// ============================================================

#define PART_BOUNDARY "123456789000000000000987654321"

static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static const char *STREAM_BOUNDARY =
    "\r\n--" PART_BOUNDARY "\r\n";

static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %zu\r\n"
    "\r\n";


// Camera configuration
static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = -1,
        .pin_reset    = -1,

        .pin_xclk     = 38,

        .pin_sccb_sda = 8,
        .pin_sccb_scl = 7,

        .pin_d7       = 21,
        .pin_d6       = 39,
        .pin_d5       = 40,
        .pin_d4       = 42,
        .pin_d3       = 46,
        .pin_d2       = 48,
        .pin_d1       = 47,
        .pin_d0       = 45,

        .pin_vsync    = 17,
        .pin_href     = 18,
        .pin_pclk     = 41,

        .xclk_freq_hz = 20000000,

        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,

        .frame_size   = FRAMESIZE_VGA,

        .jpeg_quality = 10,

        .fb_count     = 2,

        .fb_location  = CAMERA_FB_IN_PSRAM,

        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    ESP_LOGI(TAG, "Initializing OV5640 camera...");

    esp_err_t err = esp_camera_init(&config);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Camera initialization failed: 0x%x",
            err
        );

        return err;
    }

    ESP_LOGI(TAG, "OV5640 camera initialized successfully");

    return ESP_OK;
}

// Camera capture
static esp_err_t camera_capture_test(void)
{
    ESP_LOGI(TAG, "Capturing image...");

    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {
        ESP_LOGE(TAG, "Camera capture failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Capture successful!");
    ESP_LOGI(TAG, "Width      : %u", fb->width);
    ESP_LOGI(TAG, "Height     : %u", fb->height);
    ESP_LOGI(TAG, "Format     : %d", fb->format);
    ESP_LOGI(TAG, "Image size : %u bytes", (unsigned)fb->len);

    if (fb->format == PIXFORMAT_JPEG && fb->len >= 4) {

        bool jpeg_start =
            (fb->buf[0] == 0xFF &&
             fb->buf[1] == 0xD8);

        bool jpeg_end =
            (fb->buf[fb->len - 2] == 0xFF &&
             fb->buf[fb->len - 1] == 0xD9);

        ESP_LOGI(
            TAG,
            "JPEG start marker: %s",
            jpeg_start ? "OK" : "INVALID"
        );

        ESP_LOGI(
            TAG,
            "JPEG end marker: %s",
            jpeg_end ? "OK" : "INVALID"
        );

        if (!jpeg_start || !jpeg_end) {
            ESP_LOGE(TAG, "JPEG validation failed");

            esp_camera_fb_return(fb);

            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "JPEG validation successful!");
    }

    esp_camera_fb_return(fb);

    ESP_LOGI(TAG, "Frame buffer returned");

    return ESP_OK;
}

// ============================================================
// MJPEG video stream handler
// ============================================================

static esp_err_t stream_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /stream");

    esp_err_t res = httpd_resp_set_type(
        req,
        STREAM_CONTENT_TYPE
    );

    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-cache"
    );

    httpd_resp_set_hdr(
        req,
        "Access-Control-Allow-Origin",
        "*"
    );

    while (true) {

        // ----------------------------------------------------
        // Capture frame
        // ----------------------------------------------------

        camera_fb_t *fb = esp_camera_fb_get();

        if (fb == NULL) {

            ESP_LOGE(
                TAG,
                "Camera capture failed during stream"
            );

            res = ESP_FAIL;
            break;
        }

        size_t jpg_len = fb->len;
        uint8_t *jpg_buf = fb->buf;

        // Save values before returning framebuffer
        uint16_t width = fb->width;
        uint16_t height = fb->height;

        // ----------------------------------------------------
        // Send MJPEG boundary
        // ----------------------------------------------------

        res = httpd_resp_send_chunk(
            req,
            STREAM_BOUNDARY,
            strlen(STREAM_BOUNDARY)
        );

        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        // ----------------------------------------------------
        // Send JPEG headers
        // ----------------------------------------------------

        char part_buf[128];

        int hlen = snprintf(
            part_buf,
            sizeof(part_buf),
            STREAM_PART,
            jpg_len
        );

        if (hlen <= 0 || hlen >= sizeof(part_buf)) {

            ESP_LOGE(
                TAG,
                "Failed to create JPEG header"
            );

            esp_camera_fb_return(fb);
            res = ESP_FAIL;
            break;
        }

        res = httpd_resp_send_chunk(
            req,
            part_buf,
            hlen
        );

        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        // ----------------------------------------------------
        // Send JPEG
        // ----------------------------------------------------

        res = httpd_resp_send_chunk(
            req,
            (const char *)jpg_buf,
            jpg_len
        );

        // ----------------------------------------------------
        // Return framebuffer
        // ----------------------------------------------------

        esp_camera_fb_return(fb);

        if (res != ESP_OK) {

            ESP_LOGI(
                TAG,
                "Stream client disconnected"
            );

            break;
        }

        ESP_LOGI(
            TAG,
            "Stream frame sent: %ux%u, %u bytes",
            width,
            height,
            (unsigned)jpg_len
        );
    }

    return res;
}

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

// Capture Handler
static esp_err_t capture_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /capture");

    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {
        ESP_LOGE(TAG, "Camera capture failed");

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Camera capture failed"
        );

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Captured image: %ux%u, %u bytes",
        fb->width,
        fb->height,
        (unsigned)fb->len
    );

    // Tell browser that this is a JPEG image
    httpd_resp_set_type(req, "image/jpeg");

    // Prevent caching so every request captures a fresh image
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    // Send JPEG
    esp_err_t ret = httpd_resp_send(
        req,
        (const char *)fb->buf,
        fb->len
    );

    // IMPORTANT:
    // Return frame buffer only AFTER httpd_resp_send()
    esp_camera_fb_return(fb);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to send image: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    ESP_LOGI(TAG, "Image sent successfully");

    return ESP_OK;
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
        "<p><a href=\"/stream\">Video Stream Test</a></p>"
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
    // /stream
    // --------------------------------------------------------

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    ESP_ERROR_CHECK(
        httpd_register_uri_handler(
            server,
            &stream_uri
        )
    );

    // --------------------------------------------------------
    // /capture
    // --------------------------------------------------------

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

    // httpd_register_uri_handler(server, &capture_uri);

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

    ESP_LOGI(TAG, "GET  /stream");

    return server;
}

// ============================================================
// Application entry point
// ============================================================
void app_main(void)
{
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "     Shooting Target Camera");
    ESP_LOGI(TAG, "     ESP32-S3 + OV5640");
    ESP_LOGI(TAG, "====================================");

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());

        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // ----------------------------
    // Camera
    // ----------------------------

    ret = camera_init();

    if (ret != ESP_OK) {

        ESP_LOGE(TAG, "Camera initialization failed");

    } else {

        vTaskDelay(pdMS_TO_TICKS(1000));

        ret = camera_capture_test();

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera capture test failed");
        }
    }

    // ----------------------------
    // Wi-Fi
    // ----------------------------

    wifi_init_softap();

    // ----------------------------
    // HTTP server
    // ----------------------------

    start_webserver();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}