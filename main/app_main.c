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

#define TAG "fishtank"
#define MQTT_PREFIX "/fishtank"

/* TODO: LUT to compensate for perceived brightness */

typedef struct {
    nvs_handle_t nvs_handle;
    esp_mqtt_client_handle_t client;
    int16_t brightness, power;
} dimmer_data_t;

dimmer_data_t dimmers[4];
int dimmer_cnt = 1;

int dimmer_gpio[4] = {
    /* 0 */ 25
};

static void report_status(void *arg) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)arg;
    int dimmer_num = 0;

    for (dimmer_num = 0; dimmer_num < dimmer_cnt; dimmer_num++) {
        char brightness_s[32], mqtt_topic[128];
        snprintf(brightness_s, 32, "%d", dimmers[dimmer_num].brightness);
        snprintf(mqtt_topic, 128, "%s/status/%d/power", MQTT_PREFIX, dimmer_num);
        esp_mqtt_client_publish(client, mqtt_topic, dimmers[dimmer_num].power ? "ON": "OFF", 0, 0, 0);
        snprintf(mqtt_topic, 128, "%s/status/%d/brightness", MQTT_PREFIX, dimmer_num);
        esp_mqtt_client_publish(client, mqtt_topic, brightness_s, 0, 0, 0);

        ESP_LOGI(TAG, "status dimmer=%d power=%s brightness=%d", dimmer_num, dimmers[dimmer_num].power ? "ON" : "OFF", dimmers[dimmer_num].brightness);
    };
}

esp_timer_handle_t periodic_timer;

static void set_duty(esp_mqtt_client_handle_t client, int dimmer_num) {
    /* brightness = 3-255 */
    int duty = (dimmers[dimmer_num].brightness - 3) * 32; /* 0-8032 */
    if (dimmers[dimmer_num].brightness == 255) duty = 8191;

    if (dimmers[dimmer_num].power == 0) {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, dimmer_num, 0));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, dimmer_num));
    } else {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, dimmer_num, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, dimmer_num));
    }

    report_status(client);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
        {
            int dimmer_num = 0;

            esp_timer_create_args_t periodic_timer_args = {
                .callback = report_status,
                .arg = client
            };

            ESP_LOGI(TAG, "MQTT connected");

            ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
            ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 60000000)); /* every 60s */

            /* initialize dimmers */
            for(dimmer_num = 0; dimmer_num < dimmer_cnt; dimmer_num++) {
                char mqtt_topic[128];
                ledc_channel_config_t ledc_channel = {
                    .speed_mode = LEDC_HIGH_SPEED_MODE,
                    .channel = dimmer_num,
                    .timer_sel = dimmer_num,
                    .intr_type = LEDC_INTR_DISABLE,
                    .gpio_num = dimmer_gpio[dimmer_num],
                    .duty = 0,
                    .hpoint = 0
                };

                ledc_timer_config_t ledc_timer = {
                    .speed_mode = LEDC_HIGH_SPEED_MODE,
                    .timer_num = dimmer_num,
                    .duty_resolution = LEDC_TIMER_13_BIT,
                    .freq_hz = 5000,
                    .clk_cfg = LEDC_AUTO_CLK
                };

                ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

                ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

                snprintf(mqtt_topic, 128, "%s/set/%d/power", MQTT_PREFIX, dimmer_num);
                esp_mqtt_client_subscribe(client, mqtt_topic, 0);
                snprintf(mqtt_topic, 128, "%s/set/%d/brightness", MQTT_PREFIX, dimmer_num);
                esp_mqtt_client_subscribe(client, mqtt_topic, 0);

                set_duty(client, dimmer_num);
            }
        }
        break;

        case MQTT_EVENT_DATA:
        {
            int topic_offset = 0;
            int dimmer_num = 0;

            ESP_LOGI(TAG, "MQTT data topic=%.*s data=%.*s", event->topic_len, event->topic, event->data_len, event->data);

            if(strncmp(event->topic, MQTT_PREFIX "/set/", strlen(MQTT_PREFIX) + 5) != 0)
                break;
            topic_offset += strlen(MQTT_PREFIX) + 5;
            for(dimmer_num = 0; dimmer_num < dimmer_cnt; dimmer_num++) {
                if (event->topic[topic_offset] == '0' + dimmer_num) break;
            }
            if(dimmer_num == dimmer_cnt) break;

            ESP_LOGI(TAG, "dimmer=%d", dimmer_num);

            while(topic_offset < event->topic_len && event->topic[topic_offset] != '/') topic_offset++;

            if (topic_offset == event->topic_len) break;

            if (strncmp(event->topic + topic_offset, "/brightness", event->topic_len - topic_offset) == 0) {
                ESP_LOGI(TAG, "brightness");
                char b[10];
                memset(b, 0, sizeof(b));
                strncpy(b, event->data, event->data_len);

                dimmers[dimmer_num].brightness = atoi(b);

                set_duty(client, dimmer_num);
            }
            if (strncmp(event->topic + topic_offset, "/power", event->topic_len - topic_offset) == 0) {
                ESP_LOGI(TAG, "power");
                if(strncmp(event->data, "ON", 2) == 0) dimmers[dimmer_num].power = 1;
                else dimmers[dimmer_num].power = 0;

                set_duty(client, dimmer_num);
            }
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

/*    ESP_ERROR_CHECK(nvs_open(NVS_NAME, NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_get_i16(nvs_handle, "brightness", &brightness));
    ESP_ERROR_CHECK(nvs_get_i16(nvs_handle, "power", &power));
    */

    mqtt_app_start();
}
