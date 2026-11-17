#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#include "app_config.h"
#include "firmware_version.h"
#include "backend_auth.h"
#include "backoff.h"
#include "miner_status.h"
#include "mqtt_service.h"
#include "share_reject_stats.h"
#include "ota_manager.h"
#include "pwm_duty_monitor.h"
#include "tpm300a_service.h"
#include "wifi_connect.h"
#include "ws_backend_service.h"
#include "state_service.h"

static const char *TAG = "mqtt_service";

static esp_mqtt_client_handle_t s_mqtt_client;
static bool s_started;
static volatile bool s_client_running;

/*
 * Reconnects are scheduled here instead of the client's fixed 5 s retry:
 * the delay grows on failures (5 s .. 30 s) and resets once connected.
 */
#define MQTT_RECONNECT_MIN_MS 5000
#define MQTT_RECONNECT_MAX_MS 30000
static backoff_t s_reconnect_backoff = BACKOFF_INIT(MQTT_RECONNECT_MIN_MS, MQTT_RECONNECT_MAX_MS);
static esp_timer_handle_t s_reconnect_timer;

static void mqtt_reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_client_running || s_mqtt_client == NULL) {
        return;
    }
    if (esp_mqtt_client_reconnect(s_mqtt_client) != ESP_OK) {
        /* The client is not waiting yet (it is still tearing down): try again shortly. */
        (void)esp_timer_start_once(s_reconnect_timer, 1000 * 1000);
    }
}

static void mqtt_schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    uint32_t delay_ms = backoff_next_ms(&s_reconnect_backoff);
    (void)esp_timer_stop(s_reconnect_timer);
    (void)esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
    ESP_LOGW(TAG, "MQTT reconnect in %lu ms", (unsigned long)delay_ms);
}
static bool s_connected;
static int64_t s_last_send_us;
static portMUX_TYPE s_share_reject_lock = portMUX_INITIALIZER_UNLOCKED;
static share_reject_stats_t s_share_reject_stats;
static bool s_snapshot_publishing;

static char s_topic_telemetry[96];
static char s_topic_commands[96];

static const uint32_t MQTT_START_RETRY_MS = 5000;
static const size_t MQTT_MIN_INTERNAL_BLOCK = 2400;
static const uint32_t MQTT_CLIENT_TASK_STACK_SIZE = 2560;
static const uint32_t MQTT_TELEMETRY_TASK_STACK_SIZE = 4096;
// Ten 63-byte reasons can each expand sixfold when JSON-escaped.
static const size_t MQTT_STATE_PAYLOAD_SIZE = 6144;

static const char *ota_check_status_to_string(ota_check_status_t status)
{
    switch (status) {
    case OTA_CHECK_STATUS_OK:
        return "ok";
    case OTA_CHECK_STATUS_FAILED:
        return "failed";
    case OTA_CHECK_STATUS_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *ota_update_needed_to_string(bool known, bool needed)
{
    if (!known) {
        return "unknown";
    }
    return needed ? "yes" : "no";
}

static const char *ota_process_status_to_string(ota_process_status_t status)
{
    switch (status) {
    case OTA_PROCESS_STATUS_DOWNLOADING:
        return "downloading";
    case OTA_PROCESS_STATUS_READY_FOR_REBOOT:
        return "ready_for_reboot";
    case OTA_PROCESS_STATUS_FAILED:
        return "failed";
    case OTA_PROCESS_STATUS_IDLE:
    default:
        return "idle";
    }
}

void mqtt_service_record_share_reject(const char *reason)
{
    portENTER_CRITICAL(&s_share_reject_lock);
    share_reject_stats_record(&s_share_reject_stats, reason);
    portEXIT_CRITICAL(&s_share_reject_lock);
}

/* Writes template with its single "%s" replaced by device_id; false if the template has no
 * "%s" (also when it is not configured) or the result does not fit. */
static bool format_topic(char *out, size_t out_size, const char *template, const char *device_id)
{
    const char *placeholder = strstr(template, "%s");
    if (placeholder == NULL) {
        return false;
    }

    int len = snprintf(out,
                       out_size,
                       "%.*s%s%s",
                       (int)(placeholder - template),
                       template,
                       device_id,
                       placeholder + 2);
    return len > 0 && (size_t)len < out_size;
}

static bool build_topics(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return false;
    }

    return format_topic(s_topic_telemetry, sizeof(s_topic_telemetry), CONFIG_HEATBIT_MQTT_TELEMETRY_TOPIC, device_id)
           && format_topic(s_topic_commands, sizeof(s_topic_commands), CONFIG_HEATBIT_MQTT_COMMANDS_TOPIC, device_id);
}

static void log_mqtt_error(const esp_mqtt_event_handle_t event)
{
    if (event == NULL || event->error_handle == NULL) {
        ESP_LOGW(TAG, "MQTT event error without error handle");
        return;
    }

    const esp_mqtt_error_codes_t *err = event->error_handle;
    ESP_LOGW(TAG,
             "MQTT error: type=%d esp_tls_last_esp_err=0x%x esp_tls_stack_err=0x%x cert_flags=0x%x transport_errno=%d connect_rc=%d",
             (int)err->error_type,
             err->esp_tls_last_esp_err,
             err->esp_tls_stack_err,
             err->esp_tls_cert_verify_flags,
             err->esp_transport_sock_errno,
             err->connect_return_code);
}

static void log_command_message(const char *topic, int topic_len, const char *payload, int payload_len)
{
    if (topic == NULL || payload == NULL || topic_len <= 0 || payload_len < 0) {
        return;
    }

    char topic_buf[96] = {0};
    char payload_buf[256] = {0};

    int topic_copy_len = topic_len;
    if (topic_copy_len >= (int)sizeof(topic_buf)) {
        topic_copy_len = (int)sizeof(topic_buf) - 1;
    }

    int payload_copy_len = payload_len;
    if (payload_copy_len >= (int)sizeof(payload_buf)) {
        payload_copy_len = (int)sizeof(payload_buf) - 1;
    }

    memcpy(topic_buf, topic, (size_t)topic_copy_len);
    topic_buf[topic_copy_len] = '\0';

    if (payload_copy_len > 0) {
        memcpy(payload_buf, payload, (size_t)payload_copy_len);
    }
    payload_buf[payload_copy_len] = '\0';

    ESP_LOGI(TAG, "MQTT command topic=%s payload=%s", topic_buf, payload_buf);

    if (strcmp(payload_buf, "ping") == 0 && s_connected) {
        const char *pong = "{\"type\":\"pong\"}";
        int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_telemetry, pong, 0, 1, 0);
        if (msg_id >= 0) {
            s_last_send_us = esp_timer_get_time();
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = event_data;
    if (event == NULL) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        backoff_reset(&s_reconnect_backoff);
        if (s_reconnect_timer != NULL) {
            (void)esp_timer_stop(s_reconnect_timer);
        }
        ESP_LOGI(TAG,
                 "Connected to MQTT broker %s:%d",
                 CONFIG_HEATBIT_MQTT_BROKER_HOST,
                 CONFIG_HEATBIT_MQTT_BROKER_PORT);
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_commands, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        mqtt_schedule_reconnect();
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed to topic %s", s_topic_commands);
        break;
    case MQTT_EVENT_DATA:
        log_command_message(event->topic, event->topic_len, event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        log_mqtt_error(event);
        break;
    default:
        break;
    }
}

static esp_err_t build_state_payload(const state_t *snapshot, const share_reject_stats_t *reject_stats,
                                    char *out_payload, size_t out_payload_size)
{
    if (snapshot == NULL || out_payload == NULL || out_payload_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    miner_status_t miner;
    if (miner_status_get(&miner) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t timestamp = (int64_t)time(NULL);
    uint64_t uptime_s = miner.uptime_s;
    bool mining_on = miner.mining_on;
    uint64_t mining_uptime_s = miner.mining_uptime_s;

    float pwm_in = 0.0f;
    bool pwm_valid = false;
    if (pwm_duty_monitor_get_last_duty_percent(&pwm_in, &pwm_valid) != ESP_OK || !pwm_valid) {
        pwm_in = 0.0f;
    }

    bool ws_started = false;
    bool websocket_connected = false;
    int64_t ws_last_tx_us = 0;
    int64_t ws_last_rx_us = 0;
    if (ws_backend_service_get_runtime_status(&ws_started,
                                              &websocket_connected,
                                              &ws_last_tx_us,
                                              &ws_last_rx_us) != ESP_OK) {
        websocket_connected = false;
    }

    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));
    int rssi = -127;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    ota_runtime_status_t ota_status;
    memset(&ota_status, 0, sizeof(ota_status));
    ota_check_status_t ota_check_status = OTA_CHECK_STATUS_UNKNOWN;
    bool ota_update_needed_known = false;
    bool ota_update_needed = false;
    ota_process_status_t ota_process_status = OTA_PROCESS_STATUS_IDLE;
    int64_t ota_last_check_unix_s = 0;
    if (ota_manager_get_runtime_status(&ota_status) == ESP_OK) {
        ota_check_status = ota_status.check_status;
        ota_update_needed_known = ota_status.update_needed_known;
        ota_update_needed = ota_status.update_needed;
        ota_process_status = ota_status.process_status;
        ota_last_check_unix_s = ota_status.last_check_unix_s;
    }

    const char *wifi_ssid = snapshot->wifi_connected_ssid;
    const char *local_ip = miner.ip_addr;

    bool air_sensor_available = false;
    float air_sensor_tvoc = snapshot->sensor_data.voc;
    float air_sensor_co2 = snapshot->sensor_data.co2;
    float sensor_temp = snapshot->sensor_data.temp;
    float sensor_humidity = snapshot->sensor_data.humidity;

    tpm300a_snapshot_t air_snapshot;
    if (tpm300a_service_get_snapshot(&air_snapshot) == ESP_OK) {
        air_sensor_available = air_snapshot.has_values;
        if (air_snapshot.has_values) {
            air_sensor_tvoc = (float)air_snapshot.tvoc_raw;
            air_sensor_co2 = (float)air_snapshot.co2_ppm;
        }
    }

    float asic_frequency = miner.frequency_mhz;
    float vcore_mv = miner.core_voltage_mv;
    float iout_ma = miner.current_ma;
    float power_total_w = miner.power_w;
    uint32_t mhs_5s = 0;
    if (miner.hashrate_ghs > 0.0f) {
        float mhs_5s_f = miner.hashrate_ghs * 1000.0f;
        if (mhs_5s_f >= (float)UINT32_MAX) {
            mhs_5s = UINT32_MAX;
        } else {
            mhs_5s = (uint32_t)(mhs_5s_f + 0.5f);
        }
    }

    float asic_efficiency = 0.0f;
    if (miner.hashrate_ghs > 0.0f && power_total_w > 0.0f) {
        asic_efficiency = power_total_w / (miner.hashrate_ghs / 1000.0f);
    }

    uint64_t shares_count = miner.shares_accepted;
    uint64_t last_share_submitted_s = miner.last_share_submitted_s;
    uint64_t last_share_accepted_s = miner.last_share_accepted_s;
    bool stratum_connected = mining_on && miner.stratum_connected;

    cJSON *reject_reasons = share_reject_stats_to_json(reject_stats);
    if (reject_reasons == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char *reject_reasons_json = cJSON_PrintUnformatted(reject_reasons);
    cJSON_Delete(reject_reasons);
    if (reject_reasons_json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int len = snprintf(out_payload,
                       out_payload_size,
                       "{"
                       "\"uptime\":%" PRIu64 ","
                       "\"power_pin\":%d,"
                       "\"work_mode\":\"%s\","
                       "\"fan_mode\":\"%s\","
                       "\"pwm_in\":%.2f,"
                       "\"pwm_fan\":%.2f,"
                       "\"rssi\":%d,"
                       "\"mining\":%d,"
                       "\"mining_uptime\":%" PRIu64 ","
                       "\"mhs_5s\":%" PRIu32 ","
                       "\"asic_efficiency\":%.3f,"
                       "\"asic_frequency\":%.2f,"
                       "\"vcore\":%.0f,"
                       "\"iout\":%.0f,"
                       "\"power_total\":%.3f,"
                       "\"temp_chip\":%.2f,"
                       "\"temp_vr\":%.2f,"
                       "\"shares_count\":%" PRIu64 ","
                       "\"shares_submitted\":%" PRIu64 ","
                       "\"shares_accepted\":%" PRIu64 ","
                       "\"shares_rejected\":%" PRIu64 ","
                       "\"share_reject_reasons\":%s,"
                       "\"last_share_submitted\":%" PRIu64 ","
                       "\"last_share_accepted\":%" PRIu64 ","
                       "\"stratum_connected\":%d,"
                       "\"pool_difficulty\":%.17g,"
                       "\"timestamp\":%" PRIi64 ","
                       "\"app_version\":\"%s\","
                       "\"websocket_connected\":%d,"
                       "\"ota_last_check_unix_s\":%" PRIi64 ","
                       "\"ota_check_status\":\"%s\","
                       "\"ota_update_needed\":\"%s\","
                       "\"ota_process_status\":\"%s\","
                       "\"wifi_ssid\":\"%s\","
                       "\"local_ip\":\"%s\","
                       "\"sensor_temp\":%.2f,"
                       "\"sensor_humidity\":%.2f,"
                       "\"air_sensor_available\":%d,"
                       "\"air_sensor_tvoc\":%.2f,"
                       "\"air_sensor_co2\":%.2f"
                       "}",
                       uptime_s,
                       snapshot->power_pin ? 1 : 0,
                       snapshot->work_mode,
                       snapshot->fan_mode,
                       pwm_in,
                       miner.fan_perc,
                       rssi,
                       mining_on ? 1 : 0,
                       mining_uptime_s,
                       mhs_5s,
                       asic_efficiency,
                       asic_frequency,
                       vcore_mv,
                       iout_ma,
                       power_total_w,
                       miner.chip_temp_c,
                       miner.vr_temp_c,
                       shares_count,
                       miner.shares_submitted,
                       miner.shares_accepted,
                       miner.shares_rejected,
                       reject_reasons_json,
                       last_share_submitted_s,
                       last_share_accepted_s,
                       stratum_connected ? 1 : 0,
                       miner.pool_difficulty,
                       timestamp,
                       firmware_version_get_version(),
                       websocket_connected ? 1 : 0,
                       ota_last_check_unix_s,
                       ota_check_status_to_string(ota_check_status),
                       ota_update_needed_to_string(ota_update_needed_known, ota_update_needed),
                       ota_process_status_to_string(ota_process_status),
                       wifi_ssid != NULL ? wifi_ssid : "",
                       local_ip != NULL ? local_ip : "",
                       sensor_temp,
                       sensor_humidity,
                       air_sensor_available ? 1 : 0,
                       air_sensor_tvoc,
                       air_sensor_co2);
    cJSON_free(reject_reasons_json);

    if (len <= 0 || (size_t)len >= out_payload_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t mqtt_service_publish_telemetry(const char *payload, int len, int qos, int retain)
{
    if (payload == NULL || len < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_started || !s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_telemetry, payload, len, qos, retain);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    s_last_send_us = esp_timer_get_time();

    return ESP_OK;
}

static esp_err_t mqtt_service_publish_state_snapshot(void)
{
    if (!s_started || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    share_reject_stats_t reject_stats;
    portENTER_CRITICAL(&s_share_reject_lock);
    if (s_snapshot_publishing) {
        portEXIT_CRITICAL(&s_share_reject_lock);
        ESP_LOGW(TAG, "MQTT state snapshot publish is already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot_publishing = true;
    reject_stats = s_share_reject_stats;
    portEXIT_CRITICAL(&s_share_reject_lock);

    state_t snapshot = {0};
    char *payload = NULL;
    esp_err_t err = state_service_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "State snapshot is unavailable: %s", esp_err_to_name(err));
        goto done;
    }

    payload = malloc(MQTT_STATE_PAYLOAD_SIZE);
    if (payload == NULL) {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to allocate MQTT state payload");
        goto done;
    }
    err = build_state_payload(&snapshot, &reject_stats, payload, MQTT_STATE_PAYLOAD_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build MQTT payload: %s", esp_err_to_name(err));
        goto done;
    }
    err = mqtt_service_publish_telemetry(payload, 0, 1, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish MQTT state snapshot: %s", esp_err_to_name(err));
    }

done:
    free(payload);
    portENTER_CRITICAL(&s_share_reject_lock);
    if (err == ESP_OK) {
        // Subtract only the sent snapshot; rejects received during publish stay pending.
        share_reject_stats_commit(&s_share_reject_stats, &reject_stats);
    }
    s_snapshot_publishing = false;
    portEXIT_CRITICAL(&s_share_reject_lock);
    return err;
}

esp_err_t mqtt_service_get_last_send_time_us(int64_t *out_last_send_us)
{
    if (out_last_send_us == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_last_send_us = s_last_send_us;
    return ESP_OK;
}

static void mqtt_telemetry_task(void *arg)
{
    (void)arg;

    while (true) {
        bool wifi_connected = wifi_is_connected();

        if (!wifi_connected) {
            if (s_client_running && s_mqtt_client != NULL) {
                esp_err_t stop_err = esp_mqtt_client_stop(s_mqtt_client);
                if (stop_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to stop MQTT client after Wi-Fi loss: %s", esp_err_to_name(stop_err));
                }
                s_client_running = false;
                s_connected = false;
            }

            vTaskDelay(pdMS_TO_TICKS(HEATBIT_METRICS_SEND_PERIOD_MS));
            continue;
        }

        if (!s_client_running && s_mqtt_client != NULL) {
            size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (internal_largest < MQTT_MIN_INTERNAL_BLOCK) {
                ESP_LOGW(TAG,
                         "MQTT start deferred: internal heap is low (largest=%u free=%u, need~%u)",
                         (unsigned)internal_largest,
                         (unsigned)internal_free,
                         (unsigned)MQTT_MIN_INTERNAL_BLOCK);
                vTaskDelay(pdMS_TO_TICKS(MQTT_START_RETRY_MS));
                continue;
            }

            esp_err_t start_err = esp_mqtt_client_start(s_mqtt_client);
            if (start_err != ESP_OK) {
                internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                ESP_LOGW(TAG,
                         "MQTT client start deferred until Wi-Fi/internal heap is stable: %s (internal_largest=%u internal_free=%u)",
                         esp_err_to_name(start_err),
                         (unsigned)internal_largest,
                         (unsigned)internal_free);
                vTaskDelay(pdMS_TO_TICKS(MQTT_START_RETRY_MS));
                continue;
            }
            s_client_running = true;
        }

        if (s_connected) {
            esp_err_t err = mqtt_service_publish_state_snapshot();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MQTT telemetry publish failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(HEATBIT_METRICS_SEND_PERIOD_MS));
    }
}

esp_err_t mqtt_service_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    state_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    ESP_RETURN_ON_ERROR(state_service_get_snapshot(&snapshot), TAG, "state snapshot is unavailable");

    if (snapshot.device_id[0] == '\0') {
        ESP_LOGE(TAG, "Cannot start MQTT service: device_id is missing");
        return ESP_ERR_INVALID_STATE;
    }

    if (!build_topics(snapshot.device_id)) {
        ESP_LOGE(TAG, "MQTT topics are not configured or too long");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = CONFIG_HEATBIT_MQTT_BROKER_HOST,
        .broker.address.port = CONFIG_HEATBIT_MQTT_BROKER_PORT,
        .credentials.client_id = snapshot.device_id,
        .session.keepalive = 60,
        .network.timeout_ms = 10000,
        .network.disable_auto_reconnect = true,
        .task.stack_size = MQTT_CLIENT_TASK_STACK_SIZE,
    };
    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = mqtt_reconnect_timer_cb,
            .name = "mqtt_reconnect",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "reconnect timer");
    }

    if (backend_auth_mqtt(&mqtt_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT credentials are not available");
        return ESP_ERR_INVALID_STATE;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_mqtt_client,
                                                    ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler,
                                                    NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return err;
    }

    BaseType_t task_ok = xTaskCreateWithCaps(mqtt_telemetry_task,
                                             "mqtt_telemetry",
                                             MQTT_TELEMETRY_TASK_STACK_SIZE,
                                             NULL,
                                             tskIDLE_PRIORITY + 1,
                                             NULL,
                                             MALLOC_CAP_SPIRAM);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "PSRAM stack create failed for mqtt_telemetry, retrying with internal RAM");
        task_ok = xTaskCreate(mqtt_telemetry_task,
                              "mqtt_telemetry",
                              MQTT_TELEMETRY_TASK_STACK_SIZE,
                              NULL,
                              tskIDLE_PRIORITY + 1,
                              NULL);
    }

    if (task_ok != pdPASS) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG,
             "MQTT service started: host=%s port=%d telemetry=%s commands=%s",
             CONFIG_HEATBIT_MQTT_BROKER_HOST,
             CONFIG_HEATBIT_MQTT_BROKER_PORT,
             s_topic_telemetry,
             s_topic_commands);

    return ESP_OK;
}
