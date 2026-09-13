#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "SHOOTING_CAM";

#define WIFI_SSID       "Manish ShootingCam_AP"
#define WIFI_PASSWORD   "12345678"
#define WIFI_MAX_CONN   4


static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT) {

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
                    "Phone connected - MAC: %02x:%02x:%02x:%02x:%02x:%02x",
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
                    "Phone disconnected - MAC: %02x:%02x:%02x:%02x:%02x:%02x",
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
}


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


void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, " Shooting Target Camera");
    ESP_LOGI(TAG, " ESP32-S3 Starting...");
    ESP_LOGI(TAG, "=================================");

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    wifi_init_softap();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}