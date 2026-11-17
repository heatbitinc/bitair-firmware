#include "thermal_governor.h"

#include <string.h>

static float max_throttle(const thermal_governor_t *gov, float max_frequency_mhz)
{
    float span = max_frequency_mhz - gov->config.min_frequency_mhz;
    return span > 0 ? span : 0;
}

void thermal_governor_init(thermal_governor_t *gov, const thermal_governor_config_t *config)
{
    memset(gov, 0, sizeof(*gov));
    gov->config = *config;
}

bool thermal_governor_update(thermal_governor_t *gov, float chip_temp_c, float vr_temp_c, float max_frequency_mhz,
                             uint32_t now_ms)
{
    const thermal_governor_config_t *cfg = &gov->config;
    bool chip_valid = chip_temp_c >= 0;

    // The configured maximum may have been lowered meanwhile.
    float limit = max_throttle(gov, max_frequency_mhz);
    if (gov->throttle_mhz > limit) {
        gov->throttle_mhz = limit;
    }

    bool hot = (chip_valid && chip_temp_c >= cfg->chip_throttle_c) || vr_temp_c >= cfg->vr_throttle_c;
    bool cool = (!chip_valid || chip_temp_c <= cfg->chip_recover_c) && vr_temp_c <= cfg->vr_recover_c;
    uint32_t since_change = gov->has_changed ? now_ms - gov->last_change_ms : UINT32_MAX;

    float throttle = gov->throttle_mhz;
    if (hot && since_change >= cfg->down_interval_ms && throttle < limit) {
        throttle += cfg->step_mhz;
        if (throttle > limit) {
            throttle = limit;
        }
    } else if (cool && since_change >= cfg->up_interval_ms && throttle > 0) {
        throttle -= cfg->step_mhz;
        if (throttle < 0) {
            throttle = 0;
        }
    }

    if (throttle == gov->throttle_mhz) {
        return false;
    }
    gov->throttle_mhz = throttle;
    gov->last_change_ms = now_ms;
    gov->has_changed = true;
    return true;
}

void thermal_governor_on_overheat(thermal_governor_t *gov, float extra_mhz, float max_frequency_mhz, uint32_t now_ms)
{
    float throttle = gov->throttle_mhz + extra_mhz;
    float limit = max_throttle(gov, max_frequency_mhz);
    gov->throttle_mhz = throttle > limit ? limit : throttle;
    gov->last_change_ms = now_ms;
    gov->has_changed = true;
}

float thermal_governor_target_mhz(const thermal_governor_t *gov, float max_frequency_mhz)
{
    return max_frequency_mhz - gov->throttle_mhz;
}
