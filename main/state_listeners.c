#include <string.h>

#include "esp_log.h"

#include "indication_service.h"
#include "power_management_task.h"
#include "state_listeners.h"
#include "state_service.h"
#include "wifi_connect.h"
#include "ws_backend_service.h"

static const char *TAG = "state_listeners";
static GlobalState *s_global_state;

/* Mirrors Wi-Fi connectivity into GlobalState for the upstream code that reads it there. */
static void network_listener(state_change_t change, const state_t *snapshot)
{
    (void)snapshot;
    if (change != STATE_CHANGE_NETWORK || s_global_state == NULL) {
        return;
    }
    s_global_state->SYSTEM_MODULE.is_connected = wifi_connect_has_ip();
    wifi_connect_get_ip_str(s_global_state->SYSTEM_MODULE.ip_addr_str);
}

/* LED: breathing while offline, otherwise brightness follows the fan mode. */
static void indication_listener(state_change_t change, const state_t *snapshot)
{
    if (change == STATE_CHANGE_WORK_MODE) {
        return;
    }

    if (!wifi_is_connected()) {
        (void)indication_service_set_mode(INDICATION_MODE_BREATH);
        return;
    }

    if (!snapshot->power_pin || strcmp(snapshot->fan_mode, "off") == 0) {
        (void)indication_service_set_mode(INDICATION_MODE_OFF);
    } else if (strcmp(snapshot->fan_mode, "silent") == 0) {
        (void)indication_service_set_mode(INDICATION_MODE_SOLID_50);
    } else if (strcmp(snapshot->fan_mode, "turbo") == 0) {
        (void)indication_service_set_mode(INDICATION_MODE_SOLID);
    } else {
        (void)indication_service_set_mode(INDICATION_MODE_SOLID_75);
    }
}

/* Mining runs only while powered on in heater mode. */
static void mining_gate_listener(state_change_t change, const state_t *snapshot)
{
    if (change != STATE_CHANGE_POWER_PIN && change != STATE_CHANGE_WORK_MODE) {
        return;
    }

    bool mining_enabled = snapshot->power_pin && strcmp(snapshot->work_mode, "heater") == 0;
    ESP_LOGI(TAG, "Mining enabled: %s (power_pin=%s, work_mode=%s)", mining_enabled ? "true" : "false",
             snapshot->power_pin ? "true" : "false", snapshot->work_mode);
    (void)POWER_MANAGEMENT_set_mining_enabled(mining_enabled);
}

esp_err_t state_listeners_register(GlobalState *global_state)
{
    s_global_state = global_state;

    // Order matters for parity with the previous inline calls: GlobalState and
    // LED first, then the mining gate, then the backend status.
    esp_err_t err = state_service_add_listener(network_listener);
    if (err == ESP_OK) {
        err = state_service_add_listener(indication_listener);
    }
    if (err == ESP_OK) {
        err = state_service_add_listener(mining_gate_listener);
    }
    if (err == ESP_OK) {
        err = state_service_add_listener(ws_backend_service_state_listener);
    }
    return err;
}
