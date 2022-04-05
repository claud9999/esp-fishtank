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

typedef struct {
    esp_mqtt_client_handle_t client;
    int16_t time, tgt, accum, brightness, power, delta;
    bool delta_positive;
} dimmer_data_t;

dimmer_data_t dimmers[4] = { 0 };
int dimmer_cnt = 4;

int dimmer_gpio[4] = {
    12, 13, 15, 23
};

static int timer_countdown = 0;

static void set_duty(esp_mqtt_client_handle_t client, int dimmer_num) {
/*    ESP_LOGI(TAG, "set_duty dimmer=%d brightness=%d", dimmer_num, dimmers[dimmer_num].brightness); */

    if (dimmers[dimmer_num].power == 0) {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, dimmer_num, 0));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, dimmer_num));
    } else {
        /* apply dumb 2 gamma curve */
        int16_t duty = (int16_t)(((int32_t)dimmers[dimmer_num].brightness * (int32_t)dimmers[dimmer_num].brightness) / (int32_t)8192);
        if (duty > 8191) duty = 8191;
/*         ESP_LOGI(TAG, "  duty=%d", duty); */

        ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, dimmer_num, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, dimmer_num));
    }
}

static void timer_handler(void *arg) {
    int dimmer_num = 0;
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)arg;

    for (dimmer_num = 0; dimmer_num < dimmer_cnt; dimmer_num++) {
        if(dimmers[dimmer_num].time == 0) continue;

        dimmer_data_t *dimmer = &dimmers[dimmer_num];

        dimmer->accum += dimmer->delta;

        while(dimmer->accum > dimmer->time) {
            dimmer->brightness += dimmer->delta_positive ? 1 : -1;
            dimmer->accum -= dimmer->time;
        }

        if ((!dimmer->delta_positive && dimmer->brightness <= dimmer->tgt)
            || (dimmer->delta_positive && dimmer->brightness >= dimmer->tgt)) {
            dimmer->brightness = dimmer->tgt;
            dimmer->time = 0;
        }

        set_duty(client, dimmer_num);
    }

    if (timer_countdown-- > 0) return;

    timer_countdown = 600; /* reset to report in 60s */
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

static void set_bright(esp_mqtt_client_handle_t client, int dimmer_num, int16_t brightness) {
    ESP_LOGI(TAG, "set_bright dimmer=%d brightness=%d", dimmer_num, brightness);

    char nvs_name[16];
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    snprintf(nvs_name, 16, "bri/%d", dimmer_num);
    nvs_set_i16(nvs_handle, nvs_name, brightness);
    nvs_commit(nvs_handle); nvs_close(nvs_handle);

    dimmers[dimmer_num].brightness = brightness;
    dimmers[dimmer_num].tgt = brightness;
    dimmers[dimmer_num].time = 0;

    set_duty(client, dimmer_num);

    timer_countdown = 0;
}

static void ramp_bright(esp_mqtt_client_handle_t client, int dimmer_num, int16_t tgt, int16_t seconds) {
    ESP_LOGI(TAG, "ramp_bright dimmer=%d tgt=%d, seconds=%d", dimmer_num, tgt, seconds);

    dimmer_data_t *dimmer = &dimmers[dimmer_num];

    if (seconds == 0) return set_bright(client, dimmer_num, tgt);
    if (tgt == dimmer->brightness) return;

    char nvs_name[16];
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    snprintf(nvs_name, 16, "bri/%d", dimmer_num);
    nvs_set_i16(nvs_handle, nvs_name, tgt);
    nvs_commit(nvs_handle); nvs_close(nvs_handle);

    dimmer->time = seconds * 10;
    dimmer->tgt = tgt;
    dimmer->accum = 0;
    if(tgt > dimmer->brightness) {
        dimmer->delta_positive = 1;
        dimmer->delta = tgt - dimmer->brightness;
    } else {
        dimmer->delta_positive = 0;
        dimmer->delta = dimmer->brightness - tgt;
    }
}

static void set_pow(esp_mqtt_client_handle_t client, int dimmer_num, int16_t power) {
    char nvs_name[16];
    dimmers[dimmer_num].power = power;
    nvs_handle_t nvs_handle;

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    snprintf(nvs_name, 16, "pow/%d", dimmer_num);
    nvs_set_i16(nvs_handle, nvs_name, power);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    set_duty(client, dimmer_num);

    timer_countdown = 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
        {
            int dimmer_num = 0;

            esp_timer_create_args_t periodic_timer_args = {
                .callback = timer_handler,
                .arg = client
            };

            ESP_LOGI(TAG, "MQTT connected");

            ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
            ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 100000)); /* every .1s */

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

                snprintf(mqtt_topic, 128, "%s/set/%d/ramp", MQTT_PREFIX, dimmer_num);
                esp_mqtt_client_subscribe(client, mqtt_topic, 0);

                set_duty(client, dimmer_num);
                
                timer_countdown = 0;
            }
        }
        break;

        case MQTT_EVENT_DATA: {
            int offset = 0;
            int dimmer_num = 0;
            char data[32];
            int data_len = 31;

            if (event->data_len < data_len) data_len = event->data_len;
            memset(data, 0, sizeof(data));
            strncpy(data, event->data, data_len);

            ESP_LOGI(TAG, "MQTT data topic=%.*s data=%.*s", event->topic_len, event->topic, data_len, data);

            if(strncmp(event->topic, MQTT_PREFIX "/set/", strlen(MQTT_PREFIX) + 5) == 0) {
                offset += strlen(MQTT_PREFIX) + 5;
                for(dimmer_num = 0; dimmer_num < dimmer_cnt; dimmer_num++) {
                    if (event->topic[offset] == '0' + dimmer_num) break;
                }
                if(dimmer_num == dimmer_cnt) break;

                ESP_LOGI(TAG, "dimmer=%d", dimmer_num);

                while(offset < event->topic_len && event->topic[offset] != '/') offset++;

                if (offset == event->topic_len) break;

                if (strncmp(event->topic + offset, "/brightness", event->topic_len - offset) == 0) {
                    ESP_LOGI(TAG, "brightness");

                    set_bright(client, dimmer_num, atoi(data));
                }
                if (strncmp(event->topic + offset, "/power", event->topic_len - offset) == 0) {
                    ESP_LOGI(TAG, "power");
                    set_pow(client, dimmer_num, strncmp(data, "ON", 2) == 0 ? 1 : 0);
                }
                if (strncmp(event->topic + offset, "/ramp", event->topic_len - offset) == 0) {
                    ESP_LOGI(TAG, "ramp");
                    for(offset = 0;
                        data[offset] != '\0' && data[offset] != ' '; offset++) ;
                    if (data[offset] != '\0') {
                        data[offset++] = '\0';
                        ramp_bright(client, dimmer_num, atoi(data), atoi(data + offset));
                    }
                }
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
    int dimmer_num = 0;
    char nvs_name[16];

    ESP_LOGI(TAG, "app_main");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_netif_init();
    esp_event_loop_create_default();
    example_connect();

    for(dimmer_num = 0; dimmer_num < dimmer_cnt; dimmer_num++) {
        nvs_handle_t nvs_handle;

        if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
            snprintf(nvs_name, 16, "bri/%d", dimmer_num);
            if(nvs_get_i16(nvs_handle, nvs_name, &dimmers[dimmer_num].brightness) != ESP_OK) dimmers[dimmer_num].brightness = 0;
            snprintf(nvs_name, 16, "pow/%d", dimmer_num);
            if(nvs_get_i16(nvs_handle, nvs_name, &dimmers[dimmer_num].power) != ESP_OK) dimmers[dimmer_num].power = 0;
            nvs_close(nvs_handle);
        } else ESP_LOGE(TAG, "Unable to open NVS");
    }

    mqtt_app_start();
}
