#include "pwm_duty_monitor.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "state_service.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "pwm_duty";
static const gpio_num_t PWM_DUTY_INPUT_GPIO = GPIO_NUM_40;
static const float FAN_MODE_SILENT_MIN_DUTY = 25.0f;
static const float FAN_MODE_SILENT_MAX_DUTY = 65.0f;
static const float FAN_MODE_AUTO_MAX_DUTY = 85.0f;
static const uint32_t PWM_PERIOD_MAX_US = 100000; // Ignore obviously stale/invalid periods (>100 ms)
static const uint32_t PWM_SIGNAL_LOST_TIMEOUT_US = 250000; // Treat missing edges for >250ms as "no PWM signal"
static const uint32_t PWM_STARTUP_STATIC_HIGH_ACCEPT_US = 500000; // Accept static HIGH as 100% after startup grace period
static const uint32_t PWM_STATIC_HIGH_AFTER_SIGNAL_ACCEPT_US = 350000; // Accept static HIGH as 100% only after no edges for this duration
static const uint8_t FAN_MODE_DEBOUNCE_SAMPLES = 2; // Require N consecutive readings before mode switch
static const uint8_t PWM_INVALID_DEBOUNCE_SAMPLES = 2; // Require N consecutive invalid samples before declaring signal loss
static const uint32_t PWM_MONITOR_INTERVAL_MS = 250;
static const uint32_t PWM_DUTY_LOGGER_TASK_STACK_SIZE = 6144;
static const UBaseType_t PWM_DUTY_LOGGER_STACK_WARN_WORDS = 256;
static const uint32_t PWM_DUTY_LOGGER_STACK_CHECK_INTERVAL = 30;

#if CONFIG_PWM_DUTY_LOG_VERBOSE
#define PWM_LOG_VERBOSE(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define PWM_LOG_VERBOSE(...) do { } while (0)
#endif

#if CONFIG_PWM_DUTY_LOG_BASIC || CONFIG_PWM_DUTY_LOG_VERBOSE
#define PWM_LOG_BASIC(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define PWM_LOG_BASIC(...) do { } while (0)
#endif

#if CONFIG_PWM_DUTY_LOG_OFF
#define PWM_LOG_WARN(...) do { } while (0)
#else
#define PWM_LOG_WARN(...) ESP_LOGW(TAG, __VA_ARGS__)
#endif

static volatile int64_t s_pwm_last_rising_edge_us = 0;
static volatile int64_t s_pwm_last_edge_us = 0;
static volatile uint32_t s_pwm_period_us = 0;
static volatile uint32_t s_pwm_high_time_us = 0;
static volatile float s_last_duty_percent = 0.0f;
static volatile bool s_last_duty_valid = false;
static volatile bool s_pwm_seen = false;
static volatile int64_t s_pwm_monitor_start_us = 0;
static EventGroupHandle_t s_pwm_state_event_group = NULL;
static volatile bool s_pwm_monitor_enabled = false;

#define PWM_STATE_SYNCED_BIT BIT0

static esp_err_t persist_fan_and_power_state(const char *fan_mode, bool power_enabled)
{
    esp_err_t err = state_service_set_fan_mode_with_source(fan_mode, STATE_FAN_MODE_SOURCE_PWM_MONITOR);
    if (err == ESP_OK) {
        err = state_service_set_power_pin(power_enabled);
    }
    if (err == ESP_OK) {
        err = state_service_save();
    }

    return err;
}

static const char *fan_mode_for_persistence(const char *fan_mode)
{
    if (fan_mode != NULL && strcmp(fan_mode, "off") == 0) {
        return "auto";
    }

    return fan_mode;
}

static bool power_enabled_from_fan_mode(const char *fan_mode)
{
    return fan_mode == NULL || strcmp(fan_mode, "off") != 0;
}

static void pwm_duty_monitor_mark_state_synced(void)
{
    if (s_pwm_state_event_group != NULL) {
        xEventGroupSetBits(s_pwm_state_event_group, PWM_STATE_SYNCED_BIT);
    }
}

static const char *fan_mode_from_duty(float duty_percent)
{
    if (FAN_MODE_SILENT_MIN_DUTY <= duty_percent && duty_percent <= FAN_MODE_SILENT_MAX_DUTY) {
        return "silent";
    }

    if (FAN_MODE_SILENT_MAX_DUTY < duty_percent && duty_percent <= FAN_MODE_AUTO_MAX_DUTY) {
        return "auto";
    }

    if (duty_percent > FAN_MODE_AUTO_MAX_DUTY) {
        return "turbo";
    }

    return "off";
}

static void pwm_duty_input_isr_handler(void *arg)
{
    (void)arg;

    if (!s_pwm_monitor_enabled) {
        return;
    }

    int level = gpio_get_level(PWM_DUTY_INPUT_GPIO);
    int64_t now_us = esp_timer_get_time();
    s_pwm_last_edge_us = now_us;

    if (level == 1) {
        if (s_pwm_last_rising_edge_us > 0 && now_us > s_pwm_last_rising_edge_us) {
            s_pwm_period_us = (uint32_t)(now_us - s_pwm_last_rising_edge_us);
            s_pwm_seen = true;
        }
        s_pwm_last_rising_edge_us = now_us;
    } else if (s_pwm_last_rising_edge_us > 0 && now_us > s_pwm_last_rising_edge_us) {
        s_pwm_high_time_us = (uint32_t)(now_us - s_pwm_last_rising_edge_us);
        s_pwm_seen = true;
    }
}

static void pwm_duty_logger_task(void *pvParameters)
{
    (void)pvParameters;
    char last_measured_fan_mode[STATE_FAN_MODE_LEN] = {0};
    char pending_fan_mode[STATE_FAN_MODE_LEN] = {0};
    uint8_t pending_fan_mode_count = 0;
    bool last_power_enabled = false;
    bool power_state_initialized = false;
    bool last_duty_was_valid = false;
    uint8_t invalid_sample_count = 0;
    uint32_t stack_check_counter = 0;

    while (true) {
        if (!s_pwm_monitor_enabled) {
            vTaskDelay(pdMS_TO_TICKS(PWM_MONITOR_INTERVAL_MS));
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        uint32_t period_us = s_pwm_period_us;
        uint32_t high_time_us = s_pwm_high_time_us;
        int gpio_level = gpio_get_level(PWM_DUTY_INPUT_GPIO);
        int64_t last_edge_us = s_pwm_last_edge_us;
    #if CONFIG_PWM_DUTY_LOG_VERBOSE
        int64_t edge_age_us = (last_edge_us > 0 && now_us >= last_edge_us) ? (now_us - last_edge_us) : -1;
    #endif
        bool signal_stale_low = (last_edge_us > 0)
                                && ((now_us - last_edge_us) > PWM_SIGNAL_LOST_TIMEOUT_US)
                                && (gpio_level == 0);
        bool signal_stale_high = (last_edge_us > 0)
                                 && ((now_us - last_edge_us) >= PWM_STATIC_HIGH_AFTER_SIGNAL_ACCEPT_US)
                                 && (gpio_level == 1);
        bool startup_static_high_elapsed = (s_pwm_monitor_start_us > 0)
                                           && ((now_us - s_pwm_monitor_start_us) >= PWM_STARTUP_STATIC_HIGH_ACCEPT_US);
        bool duty_is_valid = (period_us > 0 && high_time_us <= period_us && period_us <= PWM_PERIOD_MAX_US);
        if (signal_stale_low) {
            duty_is_valid = false;
        }

        PWM_LOG_VERBOSE("PWM input snapshot: gpio=%d level=%d seen=%d last_edge_age_us=%" PRId64
                " high_us=%" PRIu32 " period_us=%" PRIu32 " duty_valid=%d stale_low=%d",
                PWM_DUTY_INPUT_GPIO,
                gpio_level,
                s_pwm_seen ? 1 : 0,
                edge_age_us,
                high_time_us,
                period_us,
                duty_is_valid ? 1 : 0,
                signal_stale_low ? 1 : 0);

        if (s_pwm_seen && signal_stale_high) {
            // After signal history exists, prolonged HIGH with no edges means effective 100% duty.
            // Apply this even when the previously captured duty sample still looks formally valid,
            // otherwise stale 70-80% values can persist after switching to turbo (constant HIGH).
            duty_is_valid = true;
            period_us = 1;
            high_time_us = 1;
            PWM_LOG_WARN("PWM GPIO%d static HIGH after %.0fms without edges, forcing 100%% duty",
                         PWM_DUTY_INPUT_GPIO,
                         (double) PWM_STATIC_HIGH_AFTER_SIGNAL_ACCEPT_US / 1000.0);
        } else if (!duty_is_valid && gpio_level == 1 && !s_pwm_seen && last_edge_us == 0 && startup_static_high_elapsed) {
            // If input stays HIGH for a while after boot with no edges, treat it as a valid 100% duty signal.
            // This covers controllers that output constant HIGH at 100% and therefore provide no transitions.
            duty_is_valid = true;
            period_us = 1;
            high_time_us = 1;
            PWM_LOG_WARN("PWM GPIO%d sustained HIGH at startup, interpreting as 100%% duty",
                         PWM_DUTY_INPUT_GPIO);
        } else if (!duty_is_valid && gpio_level == 1 && s_pwm_seen && !signal_stale_high) {
            PWM_LOG_VERBOSE("PWM GPIO%d HIGH but waiting for stable no-edge window before 100%% fallback",
                            PWM_DUTY_INPUT_GPIO);
        } else if (!duty_is_valid && gpio_level == 1 && !s_pwm_seen) {
            PWM_LOG_WARN("PWM GPIO%d HIGH without any captured edges yet; ignoring as invalid during startup",
                         PWM_DUTY_INPUT_GPIO);
        } else if (period_us > PWM_PERIOD_MAX_US) {
            PWM_LOG_WARN("Ignoring invalid PWM period on GPIO%d: high=%" PRIu32 "us period=%" PRIu32 "us",
                         PWM_DUTY_INPUT_GPIO,
                         high_time_us,
                         period_us);
        }

        if (duty_is_valid) {
            invalid_sample_count = 0;
            float duty_percent = ((float)high_time_us * 100.0f) / (float)period_us;
            s_last_duty_percent = duty_percent;
            s_last_duty_valid = true;
            bool signal_regained = !last_duty_was_valid;
            last_duty_was_valid = true;
            const char *measured_fan_mode = fan_mode_from_duty(duty_percent);
            const char *fan_mode_to_apply = measured_fan_mode;
            const char *persisted_fan_mode = fan_mode_for_persistence(fan_mode_to_apply);
            bool power_enabled_to_apply = power_enabled_from_fan_mode(fan_mode_to_apply);
            bool fan_mode_changed = false;

            PWM_LOG_VERBOSE("PWM GPIO%d duty: %.2f%% (high=%" PRIu32 "us, period=%" PRIu32 "us)",
                            PWM_DUTY_INPUT_GPIO,
                            duty_percent,
                            high_time_us,
                            period_us);

            if (last_measured_fan_mode[0] == '\0') {
                fan_mode_to_apply = measured_fan_mode;
                fan_mode_changed = true;
                pending_fan_mode[0] = '\0';
                pending_fan_mode_count = 0;
            } else if (strncmp(last_measured_fan_mode, measured_fan_mode, STATE_FAN_MODE_LEN) != 0) {
                if (strncmp(pending_fan_mode, measured_fan_mode, STATE_FAN_MODE_LEN) != 0) {
                    strlcpy(pending_fan_mode, measured_fan_mode, sizeof(pending_fan_mode));
                    pending_fan_mode_count = 1;
                } else if (pending_fan_mode_count < 255) {
                    pending_fan_mode_count++;
                }

                if (pending_fan_mode_count >= FAN_MODE_DEBOUNCE_SAMPLES) {
                    fan_mode_to_apply = measured_fan_mode;
                    persisted_fan_mode = fan_mode_for_persistence(fan_mode_to_apply);
                    power_enabled_to_apply = power_enabled_from_fan_mode(fan_mode_to_apply);
                    fan_mode_changed = true;
                    pending_fan_mode[0] = '\0';
                    pending_fan_mode_count = 0;
                } else {
                    // Hold last confirmed mode until debounce confirms transition.
                    fan_mode_to_apply = last_measured_fan_mode;
                    persisted_fan_mode = fan_mode_for_persistence(fan_mode_to_apply);
                    power_enabled_to_apply = power_enabled_from_fan_mode(fan_mode_to_apply);
                    PWM_LOG_VERBOSE("Debounce fan mode candidate %s (%u/%u, duty %.2f%%)",
                                    measured_fan_mode,
                                    pending_fan_mode_count,
                                    FAN_MODE_DEBOUNCE_SAMPLES,
                                    duty_percent);
                }
            } else {
                pending_fan_mode[0] = '\0';
                pending_fan_mode_count = 0;
            }

            persisted_fan_mode = fan_mode_for_persistence(fan_mode_to_apply);
            power_enabled_to_apply = power_enabled_from_fan_mode(fan_mode_to_apply);
            bool power_changed = (!power_state_initialized) || signal_regained;
            if (fan_mode_changed || power_changed) {
                esp_err_t err = persist_fan_and_power_state(persisted_fan_mode, power_enabled_to_apply);

                if (err == ESP_OK) {
                    strlcpy(last_measured_fan_mode, fan_mode_to_apply, sizeof(last_measured_fan_mode));
                    last_power_enabled = power_enabled_to_apply;
                    power_state_initialized = true;
                    PWM_LOG_BASIC("Fan mode set to %s (measured %s, duty %.2f%%), power_pin=%s power_mode=%s",
                                  persisted_fan_mode,
                                  measured_fan_mode,
                                  duty_percent,
                                  power_enabled_to_apply ? "true" : "false",
                                  power_enabled_to_apply ? "true" : "false");
                } else {
                    ESP_LOGE(TAG,
                             "Failed to persist fan/power state for mode %s: %s",
                             persisted_fan_mode,
                             esp_err_to_name(err));
                }
            }

            pwm_duty_monitor_mark_state_synced();
        } else {
            if (invalid_sample_count < 255) {
                invalid_sample_count++;
            }
            if (last_duty_was_valid && invalid_sample_count < PWM_INVALID_DEBOUNCE_SAMPLES) {
                PWM_LOG_WARN("Ignoring transient invalid PWM sample (%u/%u) before signal-loss handling",
                             invalid_sample_count,
                             PWM_INVALID_DEBOUNCE_SAMPLES);
                pwm_duty_monitor_mark_state_synced();
                stack_check_counter++;
                if (stack_check_counter >= PWM_DUTY_LOGGER_STACK_CHECK_INTERVAL) {
                    UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(NULL);
                    if (high_water_words < PWM_DUTY_LOGGER_STACK_WARN_WORDS) {
                        ESP_LOGW(TAG,
                                 "pwm_duty_logger low stack watermark: %u words",
                                 (unsigned)high_water_words);
                    }
                    stack_check_counter = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(PWM_MONITOR_INTERVAL_MS));
                continue;
            }

            s_last_duty_valid = false;
            bool signal_lost = last_duty_was_valid;
            last_duty_was_valid = false;
            pending_fan_mode[0] = '\0';
            pending_fan_mode_count = 0;

            bool power_changed = (!power_state_initialized) || last_power_enabled || signal_lost;
            if (power_changed && s_pwm_seen) {
                const char *mode_for_off = (last_measured_fan_mode[0] != '\0')
                                           ? fan_mode_for_persistence(last_measured_fan_mode)
                                           : "auto";
                esp_err_t err = persist_fan_and_power_state(mode_for_off, false);

                if (err == ESP_OK) {
                    last_power_enabled = false;
                    power_state_initialized = true;
                    PWM_LOG_BASIC("Power state set to OFF due to missing PWM signal");
                    pwm_duty_monitor_mark_state_synced();
                } else {
                    ESP_LOGE(TAG, "Failed to persist OFF power state: %s", esp_err_to_name(err));
                }
            } else if (!power_changed && s_pwm_seen) {
                pwm_duty_monitor_mark_state_synced();
            } else if (power_changed && !s_pwm_seen) {
            }
        }

        stack_check_counter++;
        if (stack_check_counter >= PWM_DUTY_LOGGER_STACK_CHECK_INTERVAL) {
            UBaseType_t high_water_words = uxTaskGetStackHighWaterMark(NULL);
            if (high_water_words < PWM_DUTY_LOGGER_STACK_WARN_WORDS) {
                ESP_LOGW(TAG,
                         "pwm_duty_logger low stack watermark: %u words",
                         (unsigned)high_water_words);
            }
            stack_check_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(PWM_MONITOR_INTERVAL_MS));
    }
}

esp_err_t pwm_duty_monitor_get_last_duty_percent(float *out_duty_percent, bool *out_valid)
{
    if (out_duty_percent == NULL || out_valid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_duty_percent = s_last_duty_percent;
    *out_valid = s_last_duty_valid;
    return ESP_OK;
}

esp_err_t pwm_duty_monitor_wait_for_state_sync(uint32_t timeout_ms)
{
    if (s_pwm_state_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_pwm_state_event_group,
                                           PWM_STATE_SYNCED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & PWM_STATE_SYNCED_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t pwm_duty_monitor_start(void)
{
    esp_err_t ret = state_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "state_service_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = state_service_load();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "state_service_load failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Before the first live PWM sample is processed, expose a safe baseline state.
    ret = state_service_set_fan_mode_with_source("auto", STATE_FAN_MODE_SOURCE_PWM_MONITOR);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set startup fan_mode=auto: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = state_service_set_power_pin(false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set startup power_pin=false: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_pwm_state_event_group == NULL) {
        s_pwm_state_event_group = xEventGroupCreate();
        if (s_pwm_state_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create PWM state event group");
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_pwm_state_event_group, PWM_STATE_SYNCED_BIT);
    s_pwm_monitor_start_us = esp_timer_get_time();

    gpio_config_t pwm_input_gpio_conf = {
        .pin_bit_mask = (1ULL << PWM_DUTY_INPUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ret = gpio_config(&pwm_input_gpio_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error configuring PWM duty input GPIO");
        return ret;
    }

    PWM_LOG_BASIC("PWM monitor configured: gpio=%d timeout_us=%" PRIu32 " period_max_us=%" PRIu32,
                  PWM_DUTY_INPUT_GPIO,
                  PWM_SIGNAL_LOST_TIMEOUT_US,
                  PWM_PERIOD_MAX_US);
    PWM_LOG_BASIC("PWM startup state: initial_level=%d seen=%d last_period_us=%" PRIu32 " last_high_us=%" PRIu32,
                  gpio_get_level(PWM_DUTY_INPUT_GPIO),
                  s_pwm_seen ? 1 : 0,
                  s_pwm_period_us,
                  s_pwm_high_time_us);

    ret = gpio_isr_handler_add(PWM_DUTY_INPUT_GPIO, pwm_duty_input_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error adding PWM duty input ISR handler");
        return ret;
    }

    if (xTaskCreate(pwm_duty_logger_task,
                    "pwm_duty_logger",
                    PWM_DUTY_LOGGER_TASK_STACK_SIZE,
                    NULL,
                    2,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating PWM duty logger task");
        return ESP_FAIL;
    }

    s_pwm_monitor_enabled = true;

    return ESP_OK;
}

esp_err_t pwm_duty_monitor_stop(void)
{
    ESP_LOGI(TAG, "Stopping PWM duty monitor");
    s_pwm_monitor_enabled = false;

    if (s_pwm_state_event_group != NULL) {
        xEventGroupClearBits(s_pwm_state_event_group, PWM_STATE_SYNCED_BIT);
    }

    ESP_LOGI(TAG, "PWM duty monitor stopped");
    return ESP_OK;
}
