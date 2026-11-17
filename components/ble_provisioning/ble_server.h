#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CU_SYSTEM_GROUP "0xA100"
#define CU_METRICS_GROUP "0xAF00"
#define CU_METRICS_MASK "0xCF00"

#define CU_SYSTEM_WIFI_STATUS "507c6e4f-db1f-45da-a649-49afc7d76751"
#define CU_SYSTEM_WIFI_STATUS_TEXT "fbd23f33-9fe0-4c95-94b8-2a1f950a8fb7"
#define CU_SYSTEM_COMMAND "def6381d-69db-4449-b091-74a90cf36da1"
#define CU_SYSTEM_CONNECT_WIFI_NAME "756c4191-a754-4206-89f4-aa3f5aadc105"
#define CU_SYSTEM_CONNECT_WIFI_PASSWORD "b2b97e9d-0c17-4273-b663-84902aac4089"

#define CMD_DISCONNECT 0x00
#define CMD_CONNECT 0x01
#define CMD_RESET 0x02
#define CMD_DONE 0x04

#define ST_NOT_CONNECTED 0x00
#define ST_CONNECTING 0x01
#define ST_CONNECTED 0x02
#define ST_CONNECTED_WITH_INTERNET 0x03
#define ST_FINISHED 0x04
#define ST_CONNECTION_ERROR 0x10
#define ST_CONNECTION_ERROR_NO_INTERNET 0x11
#define ST_BASE64_ERROR 0x12

esp_err_t ble_server_start(const char *device_id);
void ble_server_set_binding_mode(bool enabled);
esp_err_t ble_server_set_metric(uint8_t index, const char *value);
void ble_server_update_wifi_status(uint8_t status, const char *status_text);

#ifdef __cplusplus
}
#endif
