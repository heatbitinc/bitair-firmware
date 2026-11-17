#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INDICATION_MODE_OFF = 0,
    INDICATION_MODE_SOLID,
    INDICATION_MODE_SOLID_75,
    INDICATION_MODE_SOLID_50,
    INDICATION_MODE_BREATH,
    INDICATION_MODE_BLINK_FAST,
    INDICATION_MODE_BLINK_SLOW,
} indication_mode_t;

esp_err_t indication_service_start(void);
// Turns the LED off without stopping the indication service task.
esp_err_t indication_service_off(void);
esp_err_t indication_service_set_mode(indication_mode_t mode);
esp_err_t indication_service_get_mode(indication_mode_t *out_mode);

// Controls the BREATH animation speed: 1=default, 2=2x faster, etc.
esp_err_t indication_service_set_breath_speed_multiplier(uint32_t multiplier);

// Temporarily blink LED N times and then return to previous active mode.
esp_err_t indication_service_blink_fast_n(uint32_t blink_count);
esp_err_t indication_service_blink_slow_n(uint32_t blink_count);

const char *indication_service_mode_to_string(indication_mode_t mode);

#ifdef __cplusplus
}
#endif
