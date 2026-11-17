#include <string.h>

#include "freertos/FreeRTOS.h"

#include "system_status.h"

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static system_status_snapshot_t s_snapshot;

void system_status_init(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    strlcpy(s_snapshot.wifi_state, "unknown", sizeof(s_snapshot.wifi_state));
    s_snapshot.ble_running = false;
    taskEXIT_CRITICAL(&s_status_lock);
}

/* Keeps the SSID being connected to; the password is not stored. */
void system_status_set_wifi_credentials(const char *ssid, const char *password)
{
    (void)password;
    taskENTER_CRITICAL(&s_status_lock);
    strlcpy(s_snapshot.wifi_ssid, ssid != NULL ? ssid : "", sizeof(s_snapshot.wifi_ssid));
    taskEXIT_CRITICAL(&s_status_lock);
}

void system_status_set_wifi_state(const char *state)
{
    taskENTER_CRITICAL(&s_status_lock);
    strlcpy(s_snapshot.wifi_state, state != NULL ? state : "", sizeof(s_snapshot.wifi_state));
    taskEXIT_CRITICAL(&s_status_lock);
}

void system_status_set_ble_running(bool running)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_snapshot.ble_running = running;
    taskEXIT_CRITICAL(&s_status_lock);
}

void system_status_set_nvs_ids(const char *device_id, const char *worker_id)
{
    taskENTER_CRITICAL(&s_status_lock);
    strlcpy(s_snapshot.device_id, device_id != NULL ? device_id : "", sizeof(s_snapshot.device_id));
    strlcpy(s_snapshot.worker_id, worker_id != NULL ? worker_id : "", sizeof(s_snapshot.worker_id));
    taskEXIT_CRITICAL(&s_status_lock);
}

void system_status_get_snapshot(system_status_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_status_lock);
    *out = s_snapshot;
    taskEXIT_CRITICAL(&s_status_lock);
}
