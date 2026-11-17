#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log_level.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "app_config.h"
#include "firmware_version.h"
#include "miner_status.h"
#include "metrics_service.h"
#include "security_info.h"
#include "backend_auth.h"
#include "pool_config.h"
#include "state_service.h"
#include "wifi_connect.h"
#include "ws_backend_service.h"

static const char *TAG = "metrics_service";

typedef struct {
    float voc;
    float gas;
    float co2;
} metrics_sensor_data_t;

typedef struct {
    char miner_id[37];
    char ssid[33];
    uint32_t mhs_5s;
    char fan_mode[16];
    char work_mode[16];
    bool power_pin;
    metrics_sensor_data_t sensor_data;
    float miner_consumption;
} metrics_snapshot_t;

static const char *WORK_MODE_HEATER = "heater";
static const int64_t WS_STATUS_SEND_PERIOD_US = 10LL * 1000LL * 1000LL;

static bool s_metrics_task_started;

/* Single metrics task uses these buffers synchronously during HTTP POST. */
static char s_metrics_payload_buf[1536];
static char s_pool_config_payload_buf[128];

typedef struct {
    const char *name;
    char body[1024];
    size_t body_len;
    bool body_truncated;
} metrics_http_trace_t;

static metrics_http_trace_t s_metrics_http_trace = {.name = "metrics"};
static metrics_http_trace_t s_pool_config_http_trace = {.name = "pool-config"};
static int64_t s_last_metrics_send_us;

static void sync_live_mining_metrics(state_t *state_snapshot, metrics_snapshot_t *snapshot)
{
    miner_status_t miner;
    if (state_snapshot == NULL || snapshot == NULL || miner_status_get(&miner) != ESP_OK) {
        return;
    }

    float current_hashrate_ghs = miner.hashrate_ghs;
    uint32_t live_mhs_5s = 0;
    if (current_hashrate_ghs > 0.0f) {
        float mhs_5s_f = current_hashrate_ghs * 1000.0f;
        if (mhs_5s_f >= (float)UINT32_MAX) {
            live_mhs_5s = UINT32_MAX;
        } else {
            live_mhs_5s = (uint32_t)(mhs_5s_f + 0.5f);
        }
    }

    float live_miner_consumption = miner.power_w;


    ESP_LOGD(TAG, "Hashrate: %.2f, Consumption: %.2f", current_hashrate_ghs, live_miner_consumption);

    if (state_snapshot->mhs_5s != live_mhs_5s) {
        state_snapshot->mhs_5s = live_mhs_5s;
        ESP_LOGD(TAG, "SET MHS_5S %.2f", live_mhs_5s);
        state_service_set_mhs_5s(live_mhs_5s);
    }

    if (state_snapshot->miner_consumption != live_miner_consumption) {
        state_snapshot->miner_consumption = live_miner_consumption;

        ESP_LOGD(TAG, "SET MINER CONSUMPTION %.2f", live_miner_consumption);
        state_service_set_miner_consumption(live_miner_consumption);
    }

    snapshot->mhs_5s = live_mhs_5s;
    snapshot->miner_consumption = live_miner_consumption;
}

static void http_trace_reset(metrics_http_trace_t *trace)
{
    trace->body[0] = '\0';
    trace->body_len = 0;
    trace->body_truncated = false;
}

/* Collects the response body of a metrics or pool-config request into its trace. */
static esp_err_t http_trace_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->client == NULL) {
        return ESP_OK;
    }

    metrics_http_trace_t *trace = (metrics_http_trace_t *)evt->user_data;
    const char *name = trace != NULL ? trace->name : "?";

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        if (trace != NULL) {
            trace->body_len = 0;
            trace->body_truncated = false;
            trace->body[0] = '\0';
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (trace != NULL && evt->data != NULL && evt->data_len > 0) {
            size_t remaining = sizeof(trace->body) - 1 - trace->body_len;
            size_t to_copy = (size_t)evt->data_len;
            if (to_copy > remaining) {
                to_copy = remaining;
                trace->body_truncated = true;
            }
            if (to_copy > 0) {
                memcpy(trace->body + trace->body_len, evt->data, to_copy);
                trace->body_len += to_copy;
                trace->body[trace->body_len] = '\0';
            }
        }
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP %s: disconnected", name);
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP %s: event error", name);
        break;
    default:
        break;
    }

    return ESP_OK;
}

static esp_err_t build_metrics_payload(const metrics_snapshot_t *snapshot,
                                       char *payload,
                                       size_t payload_size,
                                       int *payload_len)
{
    if (snapshot == NULL || payload == NULL || payload_size == 0 || payload_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *sensor_data = cJSON_CreateObject();
    if (sensor_data == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "miner_id", snapshot->miner_id);
    cJSON_AddStringToObject(root, "ssid", snapshot->ssid);
    cJSON_AddNumberToObject(root, "mhs_5s", (double)snapshot->mhs_5s);
    cJSON_AddStringToObject(root, "fan_mode", snapshot->fan_mode);
    cJSON_AddStringToObject(root, "work_mode", snapshot->work_mode);
    cJSON_AddBoolToObject(root, "power_pin", snapshot->power_pin);
    cJSON_AddStringToObject(root, "firmware_version", firmware_version_get_version());

    cJSON_AddNumberToObject(sensor_data, "voc", (double)snapshot->sensor_data.voc);
    cJSON_AddNumberToObject(sensor_data, "gas", (double)snapshot->sensor_data.gas);
    cJSON_AddNumberToObject(sensor_data, "co2", (double)snapshot->sensor_data.co2);
    cJSON_AddItemToObject(root, "sensor_data", sensor_data);

    cJSON_AddNumberToObject(root, "miner_consumption", (int)snapshot->miner_consumption);
    miner_status_t miner;
    cJSON_AddBoolToObject(root, "temp_sensor_fault", miner_status_get(&miner) == ESP_OK && miner.temp_sensor_fault);
    security_info_add_to_json(root);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json);
    if (json_len == 0 || json_len >= payload_size) {
        cJSON_free(json);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(payload, json, json_len + 1);
    *payload_len = (int)json_len;
    cJSON_free(json);
    return ESP_OK;
}

static esp_err_t send_metrics_to_backend(const metrics_snapshot_t *snapshot)
{
    http_trace_reset(&s_metrics_http_trace);

    int payload_len = 0;
    esp_err_t err = build_metrics_payload(snapshot,
                                          s_metrics_payload_buf,
                                          sizeof(s_metrics_payload_buf),
                                          &payload_len);
    if (err != ESP_OK) {
        return err;
    }


    esp_http_client_config_t config = {
        .url = HEATBIT_API_METRICS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .user_data = &s_metrics_http_trace,
        .event_handler = http_trace_event_handler,
    };
    backend_auth_http(&config, BACKEND_HTTP_METRICS);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client for metrics POST");
        return ESP_FAIL;
    }

    err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(client, s_metrics_payload_buf, payload_len);
    }
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_perform(client);

    /* Ensure the response body is captured even if the event handler didn't
     * fire (e.g. on HTTP errors like 404). */
    if (s_metrics_http_trace.body_len == 0) {
        int read_len = esp_http_client_read(client,
                                            s_metrics_http_trace.body,
                                            sizeof(s_metrics_http_trace.body) - 1);
        if (read_len > 0) {
            s_metrics_http_trace.body_len = (size_t)read_len;
            s_metrics_http_trace.body[read_len] = '\0';
        }
    }

    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    char *content_type = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &content_type) != ESP_OK) {
        content_type = NULL;
    }

    ESP_LOGD(TAG,
             "HTTP metrics response: status=%d content_length=%lld content_type=%s body_bytes=%d body=%s%s",
             status,
             (long long)content_length,
             content_type != NULL ? content_type : "<unknown>",
             (int)s_metrics_http_trace.body_len,
             s_metrics_http_trace.body_len > 0 ? s_metrics_http_trace.body : "<empty>",
             s_metrics_http_trace.body_truncated ? " [truncated]" : "");

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Metrics POST failed: %s (HTTP status=%d)", esp_err_to_name(err), status);
        esp_http_client_cleanup(client);
        return err;
    }

    if (status >= 200 && status < 400) {
        s_last_metrics_send_us = esp_timer_get_time();
        esp_http_client_cleanup(client);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Metrics POST failed with HTTP status=%d", status);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
}

static esp_err_t fetch_pool_config_from_backend(const char *miner_id)
{
    if (miner_id == NULL || miner_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_pool_config_payload_buf, 0, sizeof(s_pool_config_payload_buf));
    http_trace_reset(&s_pool_config_http_trace);

    int payload_len = snprintf(s_pool_config_payload_buf,
                               sizeof(s_pool_config_payload_buf),
                               "miner_id=%s",
                               miner_id);
    if (payload_len <= 0 || (size_t)payload_len >= sizeof(s_pool_config_payload_buf)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = HEATBIT_API_POOL_CONFIG_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HEATBIT_POOL_CONFIG_REQUEST_TIMEOUT_MS,
        .user_data = &s_pool_config_http_trace,
        .event_handler = http_trace_event_handler,
    };
    backend_auth_http(&config, BACKEND_HTTP_POOL_CONFIG);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client for pool-config POST");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_set_header(client,
                                               "Content-Type",
                                               "application/x-www-form-urlencoded");
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(client, s_pool_config_payload_buf, payload_len);
    }
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_perform(client);

    /* Ensure the response body is captured even if the event handler didn't fire. */
    if (s_pool_config_http_trace.body_len == 0) {
        int read_len = esp_http_client_read(client,
                                            s_pool_config_http_trace.body,
                                            sizeof(s_pool_config_http_trace.body) - 1);
        if (read_len > 0) {
            s_pool_config_http_trace.body_len = (size_t)read_len;
            s_pool_config_http_trace.body[read_len] = '\0';
        }
    }

    int status = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    char *content_type = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &content_type) != ESP_OK) {
        content_type = NULL;
    }

    /* The body carries the pool password: log its size only. */
    ESP_LOGI(TAG,
             "HTTP pool-config response: status=%d content_length=%lld content_type=%s body_bytes=%d%s",
             status,
             (long long)content_length,
             content_type != NULL ? content_type : "<unknown>",
             (int)s_pool_config_http_trace.body_len,
             s_pool_config_http_trace.body_truncated ? " [truncated]" : "");

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Pool-config request failed: %s (HTTP status=%d)", esp_err_to_name(err), status);
        esp_http_client_cleanup(client);
        return err;
    }

    if (status == 200 && s_pool_config_http_trace.body_len > 0) {
        pool_config_t pool_config;
        esp_err_t apply_err = pool_config_parse(s_pool_config_http_trace.body, &pool_config);
        if (apply_err == ESP_OK) {
            apply_err = pool_config_apply(&pool_config);
        }
        if (apply_err != ESP_OK) {
            ESP_LOGW(TAG, "Pool-config apply skipped: %s", esp_err_to_name(apply_err));
        }
    }

    esp_http_client_cleanup(client);

    if (status == 200) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static void metrics_service_task(void *arg)
{
    (void)arg;

    int64_t last_ws_send_us = 0;
    int64_t last_http_send_us = 0;

    while (true) {
        ESP_LOGD(TAG, "metrics_service_task iteration start");

        /* Get current snapshot from state service */
        state_t state_snapshot;
        if (state_service_get_snapshot(&state_snapshot) != ESP_OK) {
            ESP_LOGD(TAG, "state_service_get_snapshot failed, delaying...");
            vTaskDelay(pdMS_TO_TICKS(HEATBIT_METRICS_SEND_PERIOD_MS));
            continue;
        }

        /* Convert to metrics snapshot */
        metrics_snapshot_t snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        strlcpy(snapshot.miner_id, state_snapshot.device_id, sizeof(snapshot.miner_id));
        strlcpy(snapshot.ssid, state_snapshot.wifi_connected_ssid, sizeof(snapshot.ssid));
        snapshot.mhs_5s = state_snapshot.mhs_5s;
        strlcpy(snapshot.fan_mode, state_snapshot.fan_mode, sizeof(snapshot.fan_mode));
        strlcpy(snapshot.work_mode, state_snapshot.work_mode, sizeof(snapshot.work_mode));
        snapshot.power_pin = state_snapshot.power_pin;
        memcpy(&snapshot.sensor_data, &state_snapshot.sensor_data, sizeof(snapshot.sensor_data));
        snapshot.miner_consumption = state_snapshot.miner_consumption;

        sync_live_mining_metrics(&state_snapshot, &snapshot);

        int64_t now_us = esp_timer_get_time();

        if (last_ws_send_us == 0 || (now_us - last_ws_send_us) >= WS_STATUS_SEND_PERIOD_US) {
            ESP_LOGD(TAG, "Sending status by WS: work_mode=%s fan_mode=%s power_pin=%d mhs_5s=%u miner_consumption=%.2f voc=%.2f gas=%.2f co2=%.2f",
                snapshot.work_mode,
                snapshot.fan_mode,
                snapshot.power_pin,
                snapshot.mhs_5s,
                snapshot.miner_consumption,
                state_snapshot.sensor_data.voc,
                state_snapshot.sensor_data.gas,
                state_snapshot.sensor_data.co2);
            esp_err_t ws_err = ws_backend_service_publish_status(snapshot.work_mode,
                                                                 snapshot.fan_mode,
                                                                 snapshot.power_pin,
                                                                 firmware_version_get_version(),
                                                                 snapshot.mhs_5s,
                                                                 snapshot.miner_consumption,
                                                                 state_snapshot.sensor_data.voc,
                                                                 state_snapshot.sensor_data.gas,
                                                                 state_snapshot.sensor_data.co2);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "Metrics WS publish failed: %s", esp_err_to_name(ws_err));
            }
            last_ws_send_us = now_us;
        }

        bool should_send_http =
            (last_http_send_us == 0) ||
            ((now_us - last_http_send_us) >= ((int64_t)HEATBIT_METRICS_SEND_PERIOD_MS * 1000LL));

        if (!should_send_http) {
            ESP_LOGD(TAG, "HTTP send not due yet, delaying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        last_http_send_us = now_us;

        if (!wifi_is_connected()) {
            ESP_LOGD(TAG, "Wifi not connected, delaying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGD(TAG, "Checking internet reachability to %s...", HEATBIT_API_HOST);
        bool reachable = false;
        esp_err_t reach_err = wifi_connect_check_internet_reachability(
            HEATBIT_API_HOST,
            HEATBIT_METRICS_CONNECTIVITY_TIMEOUT_MS,
            &reachable);
        ESP_LOGD(TAG, "Internet reachability check done: err=%s reachable=%d",
                 esp_err_to_name(reach_err), reachable);

        if (reach_err == ESP_OK && reachable) {
            ESP_LOGD(TAG, "Fetching pool config...");
            esp_err_t cfg_err = fetch_pool_config_from_backend(snapshot.miner_id);
            if (cfg_err != ESP_OK) {
                ESP_LOGD(TAG, "Pool-config fetch failed: %s", esp_err_to_name(cfg_err));
            }

            ESP_LOGD(TAG, "Sending metrics to backend...");
            esp_err_t send_err = send_metrics_to_backend(&snapshot);
            if (send_err != ESP_OK) {
                ESP_LOGD(TAG, "Metrics send failed: %s", esp_err_to_name(send_err));
            }
        } else {
            ESP_LOGD(TAG, "Internet not reachable, skipping HTTP send");
        }

        ESP_LOGD(TAG, "metrics_service_task iteration end, delaying 1s...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t metrics_service_init(void)
{
    /* Initialize state service if not already done */
    if (state_service_init() != ESP_OK) {
        return ESP_FAIL;
    }

    /* Set defaults in state service. The fan mode is not touched: it comes from
     * the PWM input (the display), which pwm_duty_monitor has already applied. */
    state_service_set_work_mode(WORK_MODE_HEATER);
    state_service_set_mhs_5s(0);
    state_service_set_miner_consumption(0.0f);

    return ESP_OK;
}

esp_err_t metrics_service_start(void)
{
    ESP_RETURN_ON_ERROR(metrics_service_init(), TAG, "metrics_service_init failed");

    if (s_metrics_task_started) {
        return ESP_OK;
    }

    BaseType_t task_ok = xTaskCreate(metrics_service_task,
                                     "metrics_service",
                                     8192,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     NULL);
    if (task_ok != pdPASS) {
        return ESP_FAIL;
    }

    s_metrics_task_started = true;
    return ESP_OK;
}


esp_err_t metrics_service_get_last_send_time_us(int64_t *out_last_send_us)
{
    if (out_last_send_us == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_last_send_us = s_last_metrics_send_us;
    return ESP_OK;
}
