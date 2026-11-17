#ifndef THERMAL_GOVERNOR_H_
#define THERMAL_GOVERNOR_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Thermal frequency governor.
 *
 * Keeps a throttle (MHz below the configured frequency) that grows in steps
 * while the chip or the core regulator is above its throttle temperature and
 * shrinks again while both are below their recover temperatures. The power
 * management task applies the resulting target as a partial frequency ramp,
 * without stopping mining. Pure logic: no FreeRTOS or hardware access, so it
 * can be tested on the host.
 */

typedef struct {
    float chip_throttle_c;   // start throttling at or above this chip temperature
    float chip_recover_c;    // allow speeding up at or below this chip temperature
    float vr_throttle_c;     // same for the core regulator
    float vr_recover_c;
    float step_mhz;          // frequency change per adjustment
    uint32_t down_interval_ms;  // minimum time between two slow-downs
    uint32_t up_interval_ms;    // minimum time between two speed-ups (and after a slow-down)
    float min_frequency_mhz;    // never throttle below this
} thermal_governor_config_t;

typedef struct {
    thermal_governor_config_t config;
    float throttle_mhz;
    uint32_t last_change_ms;
    bool has_changed;
} thermal_governor_t;

void thermal_governor_init(thermal_governor_t *gov, const thermal_governor_config_t *config);

/*
 * Updates the throttle from the current temperatures. chip_temp_c < 0 means no
 * chip reading (it is then ignored). Returns true if the throttle changed.
 */
bool thermal_governor_update(thermal_governor_t *gov, float chip_temp_c, float vr_temp_c, float max_frequency_mhz,
                             uint32_t now_ms);

/* After an emergency overheat stop: restart with at least this much extra throttle. */
void thermal_governor_on_overheat(thermal_governor_t *gov, float extra_mhz, float max_frequency_mhz, uint32_t now_ms);

/* Target frequency for the configured maximum, never below the minimum (unless the maximum is lower). */
float thermal_governor_target_mhz(const thermal_governor_t *gov, float max_frequency_mhz);

#endif /* THERMAL_GOVERNOR_H_ */
