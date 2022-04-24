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

#include "esp_https_ota.h"

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

#include <ds18x20.h>

#define TAG "fishtank"
#define MQTT_PREFIX "/fishtank"
#define SENSOR_GPIO 21

extern const uint8_t server_cert_pem_start[] asm("_binary_hotcat_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_hotcat_pem_end");

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

static void set_duty(int dimmer_num, int16_t duty) {
    if (duty < 0) duty = 0;
    if (duty > 8191) duty = 8191;

    ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, dimmer_num, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, dimmer_num));
}

static void update_duty(int dimmer_num) {
/*    ESP_LOGI(TAG, "update_duty dimmer=%d brightness=%d", dimmer_num, dimmers[dimmer_num].brightness); */

    if (dimmers[dimmer_num].power == 0)
        set_duty(dimmer_num, 0);
    else {
        int32_t brightness = dimmers[dimmer_num].brightness;
        set_duty(dimmer_num, ((brightness * brightness / 8192) * brightness) / 8192);
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

        update_duty(dimmer_num);
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

    dimmers[dimmer_num].brightness = brightness;
    dimmers[dimmer_num].tgt = brightness;
    dimmers[dimmer_num].time = 0;

    update_duty(dimmer_num);

    timer_countdown = 0;
}

static void ramp_bright(esp_mqtt_client_handle_t client, int dimmer_num, int16_t tgt, int16_t seconds) {
    ESP_LOGI(TAG, "ramp_bright dimmer=%d tgt=%d, seconds=%d", dimmer_num, tgt, seconds);

    dimmer_data_t *dimmer = &dimmers[dimmer_num];

    if (seconds == 0) return set_bright(client, dimmer_num, tgt);
    if (tgt == dimmer->brightness) return;

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
    dimmers[dimmer_num].power = power;

    update_duty(dimmer_num);

    timer_countdown = 0;
}

void sense_temp(esp_mqtt_client_handle_t client) {
    ds18x20_addr_t addrs[1];
    size_t sensor_count = 0;

    ESP_ERROR_CHECK(ds18x20_scan_devices(SENSOR_GPIO, addrs, 1, &sensor_count));
    if (!sensor_count) {
        ESP_LOGW(TAG, "No sensors detected");
        esp_mqtt_client_publish(client, MQTT_PREFIX "/temp", "-271", 0, 0, 0);
        return;
    }

    float temp_c;
    ds18x20_measure_and_read(SENSOR_GPIO, addrs[0], &temp_c);
    ESP_LOGI(TAG, "temp %fF (%fC)", temp_c * 1.8 + 32, temp_c);
    char temp_buf[16];
    snprintf(temp_buf, 16, "%f", temp_c);
    esp_mqtt_client_publish(client, MQTT_PREFIX "/temp", temp_buf, 0, 0, 0);
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

                snprintf(mqtt_topic, 128, "%s/set/%d/duty", MQTT_PREFIX, dimmer_num);
                esp_mqtt_client_subscribe(client, mqtt_topic, 0);

                snprintf(mqtt_topic, 128, "%s/set/%d/ramp", MQTT_PREFIX, dimmer_num);
                esp_mqtt_client_subscribe(client, mqtt_topic, 0);

                update_duty(dimmer_num);
                
                timer_countdown = 0;
            }

            esp_mqtt_client_subscribe(client, MQTT_PREFIX "/ota", 0);
            esp_mqtt_client_subscribe(client, MQTT_PREFIX "/get/temp", 0);
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

            if(strncmp(event->topic, MQTT_PREFIX "/ota", strlen(MQTT_PREFIX) + 4) == 0) {
                ESP_LOGI(TAG, "ota started");
                esp_http_client_config_t config = {
                    .url = "https://web.lan/fishtank.bin",
                    .cert_pem = (char *)server_cert_pem_start,
                    .skip_cert_common_name_check = true
                };
                esp_https_ota_config_t ota_config = {
                    .http_config = &config,
                };
                if (esp_https_ota(&ota_config) == ESP_OK) {
                    ESP_LOGI(TAG, "ota good, restarting");
                    esp_restart();
                }
            } else if(strncmp(event->topic, MQTT_PREFIX "/get/temp", strlen(MQTT_PREFIX) + 9) == 0) {
                sense_temp(client);
            } else if(strncmp(event->topic, MQTT_PREFIX "/set/", strlen(MQTT_PREFIX) + 5) == 0) {
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
                if (strncmp(event->topic + offset, "/duty", event->topic_len - offset) == 0) {
                    ESP_LOGI(TAG, "duty");

                    set_duty(dimmer_num, atoi(data));
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

    mqtt_app_start();
}
