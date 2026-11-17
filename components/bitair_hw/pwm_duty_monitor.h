#ifndef PWM_DUTY_MONITOR_H
#define PWM_DUTY_MONITOR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t pwm_duty_monitor_start(void);
esp_err_t pwm_duty_monitor_stop(void);
esp_err_t pwm_duty_monitor_get_last_duty_percent(float *out_duty_percent, bool *out_valid);
esp_err_t pwm_duty_monitor_wait_for_state_sync(uint32_t timeout_ms);

#endif
