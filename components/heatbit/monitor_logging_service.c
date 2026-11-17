#include "monitor_logging_service.h"

#include "esp_log.h"

#ifndef CONFIG_MONITOR_LOGGING_ENABLED
#define CONFIG_MONITOR_LOGGING_ENABLED 0
#endif

static const char *TAG = "monitor_logging";

static bool s_monitor_logging_enabled = CONFIG_MONITOR_LOGGING_ENABLED;

static void apply_monitor_logging_state(void)
{
    esp_log_level_set("*", s_monitor_logging_enabled ? ESP_LOG_INFO : ESP_LOG_NONE);
}

void monitor_logging_service_apply_default(void)
{
    apply_monitor_logging_state();
}

void monitor_logging_service_set_enabled(bool enabled)
{
    if (s_monitor_logging_enabled == enabled) {
        return;
    }

    s_monitor_logging_enabled = enabled;
    apply_monitor_logging_state();

    if (enabled) {
        ESP_LOGI(TAG, "Monitor logging enabled via runtime command");
    } else {
        ESP_LOGW(TAG, "Monitor logging disabled via runtime command");
    }
}

bool monitor_logging_service_is_enabled(void)
{
    return s_monitor_logging_enabled;
}