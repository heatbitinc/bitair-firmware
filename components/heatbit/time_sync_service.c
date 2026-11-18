#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "time_sync_service.h"
#include "wifi_connect.h"

static const char *TAG = "time_sync";

static bool s_started;
static bool s_sntp_initialized;

static const char *s_ntp_servers[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
};

static size_t configure_sntp_servers(void)
{
    size_t configured = 0;
    size_t desired = sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]);

#ifdef CONFIG_LWIP_SNTP_MAX_SERVERS
    size_t max_servers = (size_t)CONFIG_LWIP_SNTP_MAX_SERVERS;
#else
    size_t max_servers = 1;
#endif

    if (max_servers == 0) {
        return 0;
    }

    if (desired > max_servers) {
        desired = max_servers;
    }

    for (size_t i = 0; i < desired; ++i) {
        esp_sntp_setservername((u8_t)i, (char *)s_ntp_servers[i]);
        ESP_LOGI(TAG, "SNTP server[%d]=%s", (int)i, s_ntp_servers[i]);
        configured++;
    }

    return configured;
}

static void log_current_time(const char *prefix)
{
    time_t now = time(NULL);
    struct tm tm_now;
    memset(&tm_now, 0, sizeof(tm_now));

    if (localtime_r(&now, &tm_now) == NULL) {
        ESP_LOGW(TAG, "%s: failed to convert time", prefix);
        return;
    }

    char buf[40] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
    ESP_LOGI(TAG, "%s: %s (unix=%lld)", prefix, buf, (long long)now);
}

static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    log_current_time("SNTP sync completed");
}

static void start_or_refresh_sntp(void)
{
    if (!s_sntp_initialized) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        size_t configured = configure_sntp_servers();
        if (configured == 0) {
            ESP_LOGW(TAG, "No SNTP servers configured");
            return;
        }
        esp_sntp_set_time_sync_notification_cb(on_time_synced);
        esp_sntp_init();
        s_sntp_initialized = true;
        ESP_LOGI(TAG, "SNTP initialized with %d server(s)", (int)configured);
        return;
    }

    esp_sntp_restart();
    ESP_LOGI(TAG, "SNTP restarted after network update");
}

static void time_sync_event_handler(void *arg,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        start_or_refresh_sntp();
    }
}

esp_err_t time_sync_service_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_STA_GOT_IP,
                                                   &time_sync_event_handler,
                                                   NULL),
                        TAG,
                        "failed to register IP_EVENT handler for SNTP");

    s_started = true;

    if (wifi_is_connected()) {
        start_or_refresh_sntp();
    }

    return ESP_OK;
}
