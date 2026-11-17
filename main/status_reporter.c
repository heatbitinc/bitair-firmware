#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "firmware_version.h"
#include "global_state.h"
#include "metrics_service.h"
#include "mqtt_service.h"
#include "state_service.h"
#include "system_status.h"
#include "ws_backend_service.h"
#include "nvs_config.h"
#include "pwm_duty_monitor.h"
#include "status_reporter.h"
#include "system.h"
#include "app_config.h" // IWYU pragma: keep

#include "freertos/task.h"

static const char *TAG = "status_reporter";
static const TickType_t STATUS_PERIOD_TICKS = pdMS_TO_TICKS(10000);

static GlobalState *s_global_state;
static bool s_started;

static const char *stratum_protocol_to_string(stratum_protocol_t protocol)
{
    switch (protocol) {
    case STRATUM_PROTOCOL_V1:
        return "SV1";
    case STRATUM_PROTOCOL_V2:
        return "SV2";
    default:
        return "UNKNOWN";
    }
}

static const char *signal_quality_from_rssi(int rssi)
{
    if (rssi >= -55) {
        return "Excellent";
    }
    if (rssi >= -67) {
        return "Good";
    }
    if (rssi >= -75) {
        return "Fair";
    }
    if (rssi >= -85) {
        return "Weak";
    }
    return "Very weak";
}

static void format_uptime_hms(char *buf, size_t buf_size, uint64_t total_seconds)
{
    uint64_t days = total_seconds / (24ULL * 3600ULL);
    total_seconds %= (24ULL * 3600ULL);
    uint64_t hours = total_seconds / 3600ULL;
    total_seconds %= 3600ULL;
    uint64_t minutes = total_seconds / 60ULL;
    uint64_t seconds = total_seconds % 60ULL;

    if (days > 0ULL) {
        snprintf(buf,
                 buf_size,
                 "%" PRIu64 "d %" PRIu64 "h %" PRIu64 "m %" PRIu64 "s",
                 days,
                 hours,
                 minutes,
                 seconds);
    } else {
        snprintf(buf,
                 buf_size,
                 "%" PRIu64 "h %" PRIu64 "m %" PRIu64 "s",
                 hours,
                 minutes,
                 seconds);
    }
}

static void format_elapsed_since_us(char *buf, size_t buf_size, int64_t now_us, int64_t ts_us)
{
    if (ts_us <= 0 || now_us <= 0 || now_us < ts_us) {
        strlcpy(buf, "never", buf_size);
        return;
    }

    uint64_t elapsed_s = (uint64_t)((now_us - ts_us) / 1000000LL);
    if (elapsed_s < 60ULL) {
        snprintf(buf, buf_size, "%" PRIu64 "s ago", elapsed_s);
        return;
    }

    if (elapsed_s < 3600ULL) {
        snprintf(buf, buf_size, "%" PRIu64 "m ago", elapsed_s / 60ULL);
        return;
    }

    snprintf(buf, buf_size, "%" PRIu64 "h ago", elapsed_s / 3600ULL);
}

static void status_reporter_task(void *arg)
{
    (void)arg;

    while (true) {
        GlobalState *gs = s_global_state;
        if (gs == NULL) {
            vTaskDelay(STATUS_PERIOD_TICKS);
            continue;
        }

        SystemModule *sys = &gs->SYSTEM_MODULE;
        PowerManagementModule *pm = &gs->POWER_MANAGEMENT_MODULE;

        state_t state_snapshot;
        memset(&state_snapshot, 0, sizeof(state_snapshot));
        state_service_get_snapshot(&state_snapshot);

        system_status_snapshot_t status_snapshot;
        memset(&status_snapshot, 0, sizeof(status_snapshot));
        system_status_get_snapshot(&status_snapshot);

        wifi_ap_record_t ap_info;
        memset(&ap_info, 0, sizeof(ap_info));
        esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap_info);
        bool wifi_connected = (ap_err == ESP_OK);
        int rssi = wifi_connected ? ap_info.rssi : -127;
        const char *signal_strength = wifi_connected ? signal_quality_from_rssi(rssi) : "N/A";

        uint64_t best_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF);
        uint16_t stratum_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT);
        uint16_t stratum_tls = nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS);

        char *stratum_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL);
        char *stratum_user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER);
        char *stratum_proto_raw = nvs_config_get_string(NVS_CONFIG_STRATUM_PROTOCOL);
        stratum_protocol_t stratum_proto = stratum_protocol_from_string(stratum_proto_raw);
        const char *stratum_proto_str = stratum_protocol_to_string(stratum_proto);

        float pwm_duty = 0.0f;
        bool pwm_valid = false;
        if (pwm_duty_monitor_get_last_duty_percent(&pwm_duty, &pwm_valid) != ESP_OK) {
            pwm_valid = false;
        }
        char pwm_in_buf[16] = {0};
        if (pwm_valid) {
            snprintf(pwm_in_buf, sizeof(pwm_in_buf), "%.2f%%", pwm_duty);
        } else {
            strlcpy(pwm_in_buf, "n/a", sizeof(pwm_in_buf));
        }

        float ghs = sys->current_hashrate;
        const char *ghs_source = "instant";
        if (ghs <= 0.0f && sys->hashrate_1m > 0.0f) {
            ghs = sys->hashrate_1m;
            ghs_source = "avg_1m";
        }
        float jth = 0.0f;
        bool jth_valid = (pm->power > 0.0f && ghs > 0.0f);
        if (jth_valid) {
            jth = pm->power / (ghs / 1000.0f);
        }

        float core_v = pm->core_voltage > 0.0f ? (pm->core_voltage / 1000.0f) : 0.0f;
        float iout_a = pm->current > 0.0f ? (pm->current / 1000.0f) : 0.0f;
        float asic_power_est = core_v * iout_a;
        float power_offset = (float)gs->DEVICE_CONFIG.family.power_offset;

        int64_t now_us = esp_timer_get_time();
        uint64_t uptime_s = 0;
        if (now_us > sys->start_time) {
            uptime_s = (uint64_t)((now_us - sys->start_time) / 1000000LL);
        }

        bool mining_on = gs->ASIC_initalized && !sys->mining_paused && sys->mining_started_at_us > 0;
        uint64_t mining_uptime_s = 0;
        if (mining_on && now_us > sys->mining_started_at_us) {
            mining_uptime_s = (uint64_t)((now_us - sys->mining_started_at_us) / 1000000LL);
        }

        char uptime_buf[48] = {0};
        char mining_uptime_buf[48] = {0};
        format_uptime_hms(uptime_buf, sizeof(uptime_buf), uptime_s);
        format_uptime_hms(mining_uptime_buf, sizeof(mining_uptime_buf), mining_uptime_s);

        bool ws_started = false;
        bool ws_connected = false;
        int64_t ws_last_tx_us = 0;
        int64_t ws_last_rx_us = 0;
        if (ws_backend_service_get_runtime_status(&ws_started,
                                                  &ws_connected,
                                                  &ws_last_tx_us,
                                                  &ws_last_rx_us) != ESP_OK) {
            ws_started = false;
            ws_connected = false;
            ws_last_tx_us = 0;
            ws_last_rx_us = 0;
        }

        int64_t metrics_last_send_us = 0;
        if (metrics_service_get_last_send_time_us(&metrics_last_send_us) != ESP_OK) {
            metrics_last_send_us = 0;
        }

        int64_t mqtt_last_send_us = 0;
        if (mqtt_service_get_last_send_time_us(&mqtt_last_send_us) != ESP_OK) {
            mqtt_last_send_us = 0;
        }

        char ws_last_tx_buf[24] = {0};
        char ws_last_rx_buf[24] = {0};
        char metrics_last_send_buf[24] = {0};
        char mqtt_last_send_buf[24] = {0};
        char mqtt_last_share_submitted_buf[24] = {0};
        char mqtt_last_share_accepted_buf[24] = {0};
        format_elapsed_since_us(ws_last_tx_buf, sizeof(ws_last_tx_buf), now_us, ws_last_tx_us);
        format_elapsed_since_us(ws_last_rx_buf, sizeof(ws_last_rx_buf), now_us, ws_last_rx_us);
        format_elapsed_since_us(metrics_last_send_buf, sizeof(metrics_last_send_buf), now_us, metrics_last_send_us);
        format_elapsed_since_us(mqtt_last_send_buf, sizeof(mqtt_last_send_buf), now_us, mqtt_last_send_us);
        format_elapsed_since_us(mqtt_last_share_submitted_buf, sizeof(mqtt_last_share_submitted_buf), now_us,
                                gs->SYSTEM_MODULE.last_share_submitted_at_us);
        format_elapsed_since_us(mqtt_last_share_accepted_buf, sizeof(mqtt_last_share_accepted_buf), now_us,
                                gs->SYSTEM_MODULE.last_share_accepted_at_us);

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");
        ESP_LOGI(TAG, "| %-20s | %-46s |", "SECTION", "VALUE");
        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");

        ESP_LOGI(TAG, "| %-20s | %-46s |", "APP Version", firmware_version_get_version());
        ESP_LOGI(TAG, "| %-20s | %-46s |", "API Host", HEATBIT_API_HOST);
        ESP_LOGI(TAG, "| %-20s | %-46s |", "Uptime", uptime_buf);
        ESP_LOGI(TAG, "| %-20s | %-46s |", "Device ID", status_snapshot.device_id[0] != '\0' ? status_snapshot.device_id : "<none>");

        ESP_LOGI(TAG,
                 "| %-20s | power_pin=%s fan_mode=%s work_mode=%s",
                 "Work Mode",
                 state_snapshot.power_pin ? "true" : "false",
                 state_snapshot.fan_mode,
                 state_snapshot.work_mode);

        ESP_LOGI(TAG,
                 "| %-20s | pwm_in=%s pwm_fan=%.1f%%",
                 "PWM Status",
                 pwm_in_buf,
                 pm->fan_perc);

        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");
        ESP_LOGI(TAG, "| %-20s | %-46s |", "WiFi SSID", wifi_connected ? (char *)ap_info.ssid : "<not connected>");
        ESP_LOGI(TAG, "| %-20s | %-46s |", "WiFi Local IP", sys->ip_addr_str[0] != '\0' ? sys->ip_addr_str : "<none>");
        ESP_LOGI(TAG, "| %-20s | RSSI=%d dBm, %s", "WiFi Signal", rssi, signal_strength);

        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");
        ESP_LOGI(TAG,
                 "| %-20s | url=%s port=%u proto=%s tls=%u",
                 "Stratum",
                 stratum_url != NULL ? stratum_url : "<none>",
                 (unsigned)stratum_port,
                 stratum_proto_str,
                 (unsigned)stratum_tls);
        ESP_LOGI(TAG, "| %-20s | %-46s |", "Stratum Worker", stratum_user != NULL ? stratum_user : "<none>");

        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");
        ESP_LOGI(TAG,
                 "| %-20s | %-46s |",
                 "Mining",
                 mining_on ? "on" : "off");
        ESP_LOGI(TAG,
             "| %-20s | asic_initialized=%d paused=%d stratum_connected=%d",
             "Mining State",
             gs->ASIC_initalized ? 1 : 0,
             sys->mining_paused ? 1 : 0,
             sys->stratum_connected ? 1 : 0);
        ESP_LOGI(TAG, "| %-20s | %-46s |", "Mining Uptime", mining_uptime_buf);
        ESP_LOGI(TAG,
                 "| %-20s | GH/s=%.2f src=%s J/TH=%s best_diff=%" PRIu64,
                 "Mining Stats",
                 ghs,
                 ghs_source,
                 jth_valid ? "computed" : "n/a",
                 best_diff);
        ESP_LOGI(TAG,
             "| %-20s | cur=%.2f 1m=%.2f 10m=%.2f 1h=%.2f",
             "Hashrate Avg",
             sys->current_hashrate,
             sys->hashrate_1m,
             sys->hashrate_10m,
             sys->hashrate_1h);
        ESP_LOGI(TAG,
                 "| %-20s | submitted=%" PRIu64 " accepted=%" PRIu64 " rejected=%" PRIu64,
                 "Shares",
                 sys->shares_submitted,
                 sys->shares_accepted,
                 sys->shares_rejected);
        ESP_LOGI(TAG,
             "| %-20s | submitted=%s accepted=%s",
             "Shares Age",
             mqtt_last_share_submitted_buf,
             mqtt_last_share_accepted_buf);
        ESP_LOGI(TAG,
             "| %-20s | work_received=%" PRIu64 " queue=%u",
             "Stratum Work",
             sys->work_received,
             (unsigned)gs->stratum_queue.count);
        ESP_LOGI(TAG,
                 "| %-20s | actual=%.2fMHz target=%.2fMHz",
                 "ASIC Frequency",
                 pm->actual_frequency,
                 pm->frequency_value);
        ESP_LOGI(TAG,
                 "| %-20s | vcore=%.0fmV iout=%.0fmA asic_est=%.2fW",
                 "ASIC Rail",
                 pm->core_voltage,
                 pm->current,
                 asic_power_est);
        if (jth_valid) {
            ESP_LOGI(TAG,
                     "| %-20s | J/TH=%.2f total=%.2fW offset=%.2fW",
                     "Mining Details",
                     jth,
                     pm->power,
                     power_offset);
        } else {
            ESP_LOGI(TAG,
                     "| %-20s | total=%.2fW offset=%.2fW",
                     "Mining Details",
                     pm->power,
                     power_offset);
        }

        ESP_LOGI(TAG,
                 "| %-20s | chip=%.2fC chip2=%.2fC vr=%.2fC",
                 "Temperatures",
                 pm->chip_temp_avg,
                 pm->chip_temp2_avg,
                 pm->vr_temp);

        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");
        ESP_LOGI(TAG,
                 "| %-20s | tvoc=%.2fC gas=%.3f co2=%.2f temp=%.2fC humidity=%.2f%%",
                 "Env Sensors",
                 state_snapshot.sensor_data.voc,
                 state_snapshot.sensor_data.gas,
                 state_snapshot.sensor_data.co2,
                 state_snapshot.sensor_data.temp,
                 state_snapshot.sensor_data.humidity);

        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");
        ESP_LOGI(TAG,
                 "| %-20s | started=%d connected=%d",
                 "WebSocket",
                 ws_started,
                 ws_connected);
        ESP_LOGI(TAG,
                 "| %-20s | last_tx=%s last_rx=%s",
                 "WebSocket Times",
                 ws_last_tx_buf,
                 ws_last_rx_buf);
        ESP_LOGI(TAG,
                 "| %-20s | last_send=%s",
                 "Metrics Times",
                 metrics_last_send_buf);
        ESP_LOGI(TAG,
             "| %-20s | last_send=%s",
             "MQTT Times",
             mqtt_last_send_buf);

        ESP_LOGI(TAG, "+----------------------+------------------------------------------------+");

        if (stratum_url != NULL) {
            free(stratum_url);
        }
        if (stratum_user != NULL) {
            free(stratum_user);
        }
        if (stratum_proto_raw != NULL) {
            free(stratum_proto_raw);
        }

        vTaskDelay(STATUS_PERIOD_TICKS);
    }
}

esp_err_t status_reporter_start(GlobalState *global_state)
{
    if (global_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_global_state = global_state;
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(status_reporter_task,
                                "status_reporter",
                                6144,
                                NULL,
                                tskIDLE_PRIORITY + 1,
                                NULL);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}
