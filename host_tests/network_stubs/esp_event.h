#pragma once
#include <stdint.h>

#include "esp_err.h"
typedef int esp_event_base_t;
typedef int esp_event_handler_instance_t;
#define WIFI_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 0
#define WIFI_EVENT_STA_CONNECTED 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 0
#define IP_EVENT_GOT_IP6 1
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              void (*handler)(void *, esp_event_base_t, int32_t,
                                                              void *),
                                              void *arg, esp_event_handler_instance_t *instance);
