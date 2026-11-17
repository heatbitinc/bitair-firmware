#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read-only snapshot of the miner for telemetry. The application (main/)
 * registers a provider that fills it from its own state, so telemetry
 * services do not depend on the miner's internals.
 */
typedef struct {
    uint64_t uptime_s;
    bool mining_on;
    uint64_t mining_uptime_s;
    float hashrate_ghs;
    float frequency_mhz;
    float core_voltage_mv;
    float current_ma;
    float power_w;
    float fan_perc;
    float chip_temp_c;
    float vr_temp_c;
    bool temp_sensor_fault;
    uint64_t shares_submitted;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    uint64_t last_share_submitted_s; /* seconds ago, 0 if none */
    uint64_t last_share_accepted_s;
    bool stratum_connected;
    double pool_difficulty;
    char ip_addr[16];
} miner_status_t;

typedef void (*miner_status_provider_t)(miner_status_t *out);

void miner_status_set_provider(miner_status_provider_t provider);

/* ESP_ERR_INVALID_STATE until a provider is registered. */
esp_err_t miner_status_get(miner_status_t *out);

#ifdef __cplusplus
}
#endif
