#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
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

#include "driver/sigmadelta.h"

sigmadelta_config_t sigmadelta_cfg = {
    .channel = SIGMADELTA_CHANNEL_0,
    .sigmadelta_prescale = 80,
    .sigmadelta_duty = -127,
    .sigmadelta_gpio = 25,
};

int duty = -127;

static void set_duty(esp_mqtt_client_handle_t client, char *new_duty) {
    duty = atoi(new_duty) - 128;
    sigmadelta_set_duty(0, duty);
    esp_mqtt_client_publish(client, "/fishtank/duty", new_duty, 0, 1, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            esp_mqtt_client_subscribe(client, "/fishtank/set", 0);

            set_duty(client, "0");

            break;

        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, "/fishtank/set", event->topic_len) == 0) {
                char b[10];
                memset(b, 0, sizeof(b));
                strncpy(b, event->data, event->data_len);

                set_duty(client, b);
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
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    example_connect();

    sigmadelta_config(&sigmadelta_cfg);

    mqtt_app_start();
}
