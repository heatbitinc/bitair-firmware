#include <string.h>

#include "esp_timer.h"

#include "miner_status.h"
#include "miner_status_provider.h"

static GlobalState *s_global_state;

static uint64_t seconds_since(int64_t timestamp_us, int64_t now_us)
{
    if (timestamp_us <= 0 || now_us <= timestamp_us) {
        return 0;
    }
    return (uint64_t)((now_us - timestamp_us) / 1000000LL);
}

static void fill(miner_status_t *out)
{
    const GlobalState *gs = s_global_state;
    const SystemModule *sys = &gs->SYSTEM_MODULE;
    const PowerManagementModule *pm = &gs->POWER_MANAGEMENT_MODULE;
    int64_t now_us = esp_timer_get_time();

    out->uptime_s = seconds_since(sys->start_time, now_us);
    out->mining_on = gs->ASIC_initalized && !sys->mining_paused && sys->mining_started_at_us > 0;
    out->mining_uptime_s = out->mining_on ? seconds_since(sys->mining_started_at_us, now_us) : 0;
    out->hashrate_ghs = sys->current_hashrate;
    out->frequency_mhz = pm->frequency_value;
    out->core_voltage_mv = pm->core_voltage;
    out->current_ma = pm->current;
    out->power_w = pm->power;
    out->fan_perc = pm->fan_perc;
    out->chip_temp_c = pm->chip_temp_avg;
    out->vr_temp_c = pm->vr_temp;
    out->temp_sensor_fault = sys->temp_sensor_fault;
    out->shares_submitted = sys->shares_submitted;
    out->shares_accepted = sys->shares_accepted;
    out->shares_rejected = sys->shares_rejected;
    out->last_share_submitted_s = seconds_since(sys->last_share_submitted_at_us, now_us);
    out->last_share_accepted_s = seconds_since(sys->last_share_accepted_at_us, now_us);
    out->stratum_connected = sys->stratum_connected;
    out->pool_difficulty = gs->pool_difficulty;
    strlcpy(out->ip_addr, sys->ip_addr_str, sizeof(out->ip_addr));
}

void miner_status_provider_register(GlobalState *global_state)
{
    s_global_state = global_state;
    miner_status_set_provider(fill);
}
