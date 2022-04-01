#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "driver/ledc.h"

const char *TAG = "fishtank";

ledc_timer_config_t ledc_timer = {
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .timer_num = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_13_BIT,
    .freq_hz = 5000,
    .clk_cfg = LEDC_AUTO_CLK
};

ledc_channel_config_t ledc_channel = {
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .channel = LEDC_CHANNEL_0,
    .timer_sel = LEDC_TIMER_0,
    .intr_type = LEDC_INTR_DISABLE,
    .gpio_num = 25,
    .duty = 0,
    .hpoint = 0
};

/* TODO: LUT to compensate for perceived brightness */

int brightness = 0, power = 0;

static void report_status(void *arg) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)arg;
    char brightness_s[32];
    snprintf(brightness_s, 32, "%d", brightness);
    ESP_LOGI(TAG, "status power=%s brightness=%d", power ? "ON" : "OFF", brightness);
    esp_mqtt_client_publish(client, "/fishtank/status/power", power ? "ON": "OFF", 0, 0, 0);
    esp_mqtt_client_publish(client, "/fishtank/status/brightness", brightness_s, 0, 0, 0);
}

esp_timer_handle_t periodic_timer;

static void set_duty(esp_mqtt_client_handle_t client) {
    /* brightness = 3-255 */
    int duty = (brightness - 3) * 32; /* 0-8032 */
    if (brightness == 255) duty = 8191;

    if (power == 0) {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, 0));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0));
    } else {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0));
    }

    report_status(client);
}

static void set_on(esp_mqtt_client_handle_t client) {
    power = 1;

    set_duty(client);

    report_status(client);
}

static void set_off(esp_mqtt_client_handle_t client) {
    power = 0;

    set_duty(client);

    report_status(client);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
        {
            esp_timer_create_args_t periodic_timer_args = {
                .callback = report_status,
                .arg = client
            };

            ESP_LOGI(TAG, "MQTT connected");

            ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
            ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

            ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
            ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 60000000)); /* every 60s */

            esp_mqtt_client_subscribe(client, "/fishtank/set/power", 0);
            esp_mqtt_client_subscribe(client, "/fishtank/set/brightness", 0);

            brightness = 0;
            set_duty(client);

        }
        break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data topic=%.*s data=%.*s", event->topic_len, event->topic, event->data_len, event->data);

            if (strncmp(event->topic, "/fishtank/set/brightness", event->topic_len) == 0) {
                char b[10];
                memset(b, 0, sizeof(b));
                strncpy(b, event->data, event->data_len);

                brightness = atoi(b);
                set_duty(client);
            }
            if (strncmp(event->topic, "/fishtank/set/power", event->topic_len) == 0) {
                if(strncmp(event->data, "ON", 2) == 0) set_on(client);
                else set_off(client);
            }
            break;

        default:
            break;
    }
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main");

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    example_connect();

    mqtt_app_start();
}
