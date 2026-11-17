#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "global_state.h"
#include "nvs_config.h"
#include "pool_config.h"
#include "pool_config_applier.h"
#include "protocol_coordinator.h"

static const char *TAG = "pool_config_applier";

static bool update_string(NvsConfigKey key, const char *value, const char *what, bool log_values)
{
    char *current = nvs_config_get_string(key);
    bool changed = current == NULL || strcmp(current, value) != 0;
    if (changed) {
        nvs_config_set_string(key, value);
        if (log_values) {
            ESP_LOGI(TAG, "Applied backend %s: old=%s new=%s", what, current != NULL ? current : "<null>", value);
        } else {
            ESP_LOGI(TAG, "Applied backend %s: value updated", what);
        }
    }
    free(current);
    return changed;
}

static bool update_u16(NvsConfigKey key, uint16_t value, const char *what)
{
    uint16_t current = nvs_config_get_u16(key);
    if (current == value) {
        return false;
    }
    nvs_config_set_u16(key, value);
    ESP_LOGI(TAG, "Applied backend %s: old=%u new=%u", what, (unsigned)current, (unsigned)value);
    return true;
}

/* Writes only what changed (NVS wear) and restarts mining when stratum settings changed. */
static esp_err_t apply(const pool_config_t *config)
{
    bool stratum_changed = false;

    if (config->has_pool) {
        stratum_changed |= update_string(NVS_CONFIG_STRATUM_URL, config->host, "pool host", true);
        stratum_changed |= update_u16(NVS_CONFIG_STRATUM_PORT, config->port, "pool port");
        stratum_changed |= update_string(NVS_CONFIG_STRATUM_PROTOCOL, STRATUM_V1, "pool protocol", true);
        stratum_changed |= update_u16(NVS_CONFIG_STRATUM_TLS, config->tls, "pool TLS mode");
    }
    if (config->has_worker) {
        stratum_changed |= update_string(NVS_CONFIG_STRATUM_USER, config->worker, "worker", true);
    }
    if (config->has_password) {
        stratum_changed |= update_string(NVS_CONFIG_STRATUM_PASS, config->password, "pool password", false);
    }
    if (config->has_power_limit) {
        (void)update_u16(NVS_CONFIG_POWER_CONSUMPTION_TARGET, config->power_limit_w, "PSU power limit");
    }

    if (stratum_changed) {
        ESP_LOGW(TAG, "Stratum settings changed from backend, forcing full mining restart");
        protocol_coordinator_request_reconnect();
    }
    return ESP_OK;
}

void pool_config_applier_register(void)
{
    pool_config_set_applier(apply);
}
