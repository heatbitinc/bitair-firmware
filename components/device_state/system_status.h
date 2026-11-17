#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_state[32];
    bool ble_running;
    char device_id[64];
    char worker_id[32];
} system_status_snapshot_t;

void system_status_init(void);

void system_status_set_wifi_credentials(const char *ssid, const char *password);
void system_status_set_wifi_state(const char *state);
void system_status_set_ble_running(bool running);
void system_status_set_nvs_ids(const char *device_id, const char *worker_id);

void system_status_get_snapshot(system_status_snapshot_t *out);

#ifdef __cplusplus
}
#endif
