#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "fan_controller_task.h"
#include "thermal.h"
#include "state_service.h"

#define EPSILON 0.0001f
#define POLL_TIME_MS 100

static const char * TAG = "fan_controller";
static const char * MODE_SILENT = "silent";
static const char * MODE_AUTO = "auto";
static const char * MODE_TURBO = "turbo";
static char prev_context[STATE_FAN_MODE_LEN] = "unknown";

static float fan_target_from_mode(const char *mode)
{
    if (strcmp(mode, MODE_SILENT) == 0) {
        return 50.0f;
    }

    if (strcmp(mode, MODE_TURBO) == 0) {
        return 100.0f;
    }

    return 75.0f;
}

void update_fan_speed(GlobalState * GLOBAL_STATE, float target_perc, const char * context)
{
    if (target_perc > 100.0f) target_perc = 100.0f;
    if (target_perc < 0.0f) target_perc = 0.0f;

    float current = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_perc;
    bool target_changed = (current > (target_perc + EPSILON)) || (current < (target_perc - EPSILON));
    if (strcmp(context, prev_context) != 0) {
        strlcpy(prev_context, context, sizeof(prev_context));
        ESP_LOGI(TAG, "Set to %s mode, fan speed: %.1f%%", context, target_perc);
    } else {
        if (target_changed) {
            ESP_LOGI(TAG, "%s mode, fan speed: %.1f%%", context, target_perc);
        }
    }
    if (target_changed) {
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_perc = target_perc;
        if (Thermal_set_fan_percent(&GLOBAL_STATE->DEVICE_CONFIG, target_perc / 100.0f) != ESP_OK) {
            ESP_LOGE(TAG, "FATAL: Fan Control Failed (%s). Flagging hardware fault.", context);
            GLOBAL_STATE->SYSTEM_MODULE.hardware_fault = true;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg, sizeof(GLOBAL_STATE->SYSTEM_MODULE.hardware_fault_msg), "Fan Control Failed (%s)", context);
        }
    }
}

void FAN_CONTROLLER_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Starting");

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;
    char fan_mode[STATE_FAN_MODE_LEN] = {0};
    bool power_pin = false;

    esp_err_t err = state_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state_service_init failed: %s", esp_err_to_name(err));
    }
    // Do not reload from NVS here: app_main already loaded state once and startup
    // services may have adjusted runtime-safe values (e.g. power_pin=false).
    // A second load in this task can resurrect stale persisted power/fan state.

    TickType_t taskWakeTime = xTaskGetTickCount();

    while (1) {
        bool power_state_ok = true;
        if (state_service_get_power_pin(&power_pin) != ESP_OK) {
            power_state_ok = false;
            ESP_LOGW(TAG, "Failed to read power_pin from state_service, forcing fan OFF");
                }

        if (!power_state_ok || !power_pin) {
            update_fan_speed(GLOBAL_STATE, 0.0f, "power_off");

            // Fan tach feedback is not available on the current hardware.
            GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_rpm = 0;
            GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan2_rpm = 0;

            vTaskDelayUntil(&taskWakeTime, POLL_TIME_MS / portTICK_PERIOD_MS);
            continue;
                    }
                    
        if (state_service_get_fan_mode(fan_mode) != ESP_OK) {
            strlcpy(fan_mode, MODE_AUTO, sizeof(fan_mode));
            ESP_LOGW(TAG, "Failed to read fan_mode from state_service, fallback to auto");
                    }
                    
        if (strcmp(fan_mode, MODE_SILENT) != 0 && strcmp(fan_mode, MODE_AUTO) != 0 && strcmp(fan_mode, MODE_TURBO) != 0) {
            ESP_LOGW(TAG, "Unknown fan_mode '%s', fallback to auto", fan_mode);
            strlcpy(fan_mode, MODE_AUTO, sizeof(fan_mode));
        }

        update_fan_speed(GLOBAL_STATE, fan_target_from_mode(fan_mode), fan_mode);

        // Fan tach feedback is not available on the current hardware.
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan_rpm = 0;
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.fan2_rpm = 0;

        vTaskDelayUntil(&taskWakeTime, POLL_TIME_MS / portTICK_PERIOD_MS);
    }
}
