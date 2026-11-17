#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "backend_auth.h"
#include "backoff.h"
#include "firmware_version.h"
#include "monitor_logging_service.h"
#include "state_service.h"
#include "system_status.h"
#include "ws_backend_service.h"

static const char *TAG = "ws_backend";

typedef enum {
    WS_STATUS_MESSAGE_AIR_QUALITY = 0,
    WS_STATUS_MESSAGE_MINER,
    WS_STATUS_MESSAGE_POWER,
    WS_STATUS_MESSAGE_COUNT,
} ws_status_message_kind_t;

static esp_websocket_client_handle_t s_ws_client;
static bool s_ws_started;
static char s_ws_headers[512];
static int64_t s_received_at_us;
static char s_pending_status[WS_STATUS_MESSAGE_COUNT][256];
static bool s_pending_status_valid[WS_STATUS_MESSAGE_COUNT];
static char s_last_status[WS_STATUS_MESSAGE_COUNT][256];
static bool s_last_status_valid[WS_STATUS_MESSAGE_COUNT];
static bool s_ws_connected;
static TaskHandle_t s_status_log_task;
static char s_rx_text_buf[1024];
static size_t s_rx_text_len;
static bool s_rx_collecting_text;
static int64_t s_last_disconnect_us;
static int64_t s_last_reconnect_attempt_us;
/* Reconnect delay: grows on failures (5 s .. 30 s), reset on a successful connection. */
#define WS_RECONNECT_MIN_MS 5000
#define WS_RECONNECT_MAX_MS 30000
static backoff_t s_reconnect_backoff = BACKOFF_INIT(WS_RECONNECT_MIN_MS, WS_RECONNECT_MAX_MS);
static uint32_t s_recreate_gap_ms;
static int64_t s_last_tx_us;

enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT = 0x1,
    WS_OPCODE_BINARY = 0x2,
    WS_OPCODE_CLOSE = 0x8,
};

static const uint32_t WS_TASK_STACK_SIZE = 8192;
static const uint32_t WS_TASK_INTERNAL_HEADROOM = 2048;
static const uint32_t WS_TASK_STACK_FALLBACK_1 = 6144;
static const uint32_t WS_TASK_STACK_FALLBACK_2 = 5120;

static esp_err_t build_ws_headers(char *headers, size_t headers_size);
static void ws_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data);
static esp_err_t send_text_message(const char *message);

/*
 * s_ws_client is created and destroyed by the reconnect watchdog and stop(),
 * while other tasks (metrics, state changes, the websocket task itself) send
 * through it. The lock covers the handle and every use of it. Stopping a
 * client waits for the websocket task, which may be blocked on this lock in
 * its event handler, so a client is detached under the lock and stopped
 * outside of it.
 */
static SemaphoreHandle_t ws_lock_handle(void)
{
    static StaticSemaphore_t s_lock_buf;
    static SemaphoreHandle_t s_lock;
    static portMUX_TYPE s_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

    if (s_lock == NULL) {
        taskENTER_CRITICAL(&s_lock_init_mux);
        if (s_lock == NULL) {
            s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
        }
        taskEXIT_CRITICAL(&s_lock_init_mux);
    }
    return s_lock;
}

static void ws_lock(void)
{
    xSemaphoreTakeRecursive(ws_lock_handle(), portMAX_DELAY);
}

static void ws_unlock(void)
{
    xSemaphoreGiveRecursive(ws_lock_handle());
}

/* Detaches the current client so no task can use it any more; returns it for destruction. */
static esp_websocket_client_handle_t ws_detach_client(void)
{
    ws_lock();
    esp_websocket_client_handle_t client = s_ws_client;
    s_ws_client = NULL;
    ws_unlock();
    return client;
}

/* Must not be called from the websocket task or with the lock held. */
static void ws_destroy_client(esp_websocket_client_handle_t client)
{
    if (client != NULL) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
}

typedef enum {
    CMD_ABSENT,  /* the message does not carry this command */
    CMD_APPLIED,
    CMD_FAILED,
} cmd_result_t;

/* Acknowledges a command after it ran: success reflects the result; error says why it failed. */
static void send_backend_command_ack(const cJSON *message, bool success, const char *error)
{
    if (message == NULL) {
        return;
    }

    char private_id[STATE_UUID_LEN] = {0};
    state_t state_snapshot;
    memset(&state_snapshot, 0, sizeof(state_snapshot));
    if (state_service_get_snapshot(&state_snapshot) == ESP_OK) {
        strlcpy(private_id, state_snapshot.device_id, sizeof(private_id));
    } else {
        ESP_LOGW(TAG, "Failed to read state snapshot for ACK private_id");
    }

    cJSON *ack = cJSON_Duplicate((cJSON *)message, 1);
    if (ack == NULL) {
        ESP_LOGW(TAG, "Failed to allocate command ACK JSON");
        return;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(ack, "success");
    cJSON_AddBoolToObject(ack, "success", success);
    cJSON_DeleteItemFromObjectCaseSensitive(ack, "error");
    if (!success && error != NULL) {
        cJSON_AddStringToObject(ack, "error", error);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(ack, "miner_id");
    cJSON_AddStringToObject(ack, "miner_id", private_id);

    char *ack_str = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (ack_str == NULL) {
        ESP_LOGW(TAG, "Failed to serialize command ACK JSON");
        return;
    }

    esp_err_t ack_err = send_text_message(ack_str);
    if (ack_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send backend command ACK: %s", esp_err_to_name(ack_err));
    }
    cJSON_free(ack_str);
}

static cmd_result_t apply_backend_fan_mode_command(const cJSON *message, const char **error)
{
    if (message == NULL) {
        return CMD_ABSENT;
    }

    const cJSON *fan_mode_item = cJSON_GetObjectItemCaseSensitive((cJSON *)message, "fan_mode");
    if (fan_mode_item == NULL) {
        return CMD_ABSENT;
    }
    if (!cJSON_IsString(fan_mode_item) || fan_mode_item->valuestring == NULL) {
        *error = "invalid fan_mode";
        return CMD_FAILED;
    }

    const cJSON *task_uuid_item = cJSON_GetObjectItemCaseSensitive((cJSON *)message, "task_uuid");
    const char *task_uuid = (cJSON_IsString(task_uuid_item) && task_uuid_item->valuestring != NULL)
                                ? task_uuid_item->valuestring
                                : "<none>";
    const char *fan_mode = fan_mode_item->valuestring;

    ESP_LOGI(TAG, "WS command: fan_mode=%s task_uuid=%s", fan_mode, task_uuid);

    if (strcmp(fan_mode, "off") == 0) {
        esp_err_t err_mode = state_service_set_fan_mode_with_source("off", STATE_FAN_MODE_SOURCE_REMOTE_CONTROL);
        esp_err_t err_pin = state_service_set_power_pin(false);
        if (err_mode != ESP_OK || err_pin != ESP_OK) {
            ESP_LOGW(TAG,
                     "Failed to apply fan_mode=off command: fan_mode=%s power_pin=%s",
                     esp_err_to_name(err_mode),
                     esp_err_to_name(err_pin));
            *error = "fan_mode not applied";
            return CMD_FAILED;
        }
        return CMD_APPLIED;
    }

    if (strcmp(fan_mode, "auto") == 0 || strcmp(fan_mode, "silent") == 0 || strcmp(fan_mode, "turbo") == 0) {
        esp_err_t err_mode = state_service_set_fan_mode_with_source(fan_mode, STATE_FAN_MODE_SOURCE_REMOTE_CONTROL);
        esp_err_t err_pin = state_service_set_power_pin(true);
        if (err_mode != ESP_OK || err_pin != ESP_OK) {
            ESP_LOGW(TAG,
                     "Failed to apply fan_mode=%s command: fan_mode=%s power_pin=%s",
                     fan_mode,
                     esp_err_to_name(err_mode),
                     esp_err_to_name(err_pin));
            *error = "fan_mode not applied";
            return CMD_FAILED;
        }
        return CMD_APPLIED;
    }

    ESP_LOGW(TAG, "Unsupported fan_mode command: %s", fan_mode);
    *error = "unsupported fan_mode";
    return CMD_FAILED;
}

static cmd_result_t apply_backend_work_mode_command(const cJSON *message, const char **error)
{
    if (message == NULL) {
        return CMD_ABSENT;
    }

    const cJSON *work_mode_item = cJSON_GetObjectItemCaseSensitive((cJSON *)message, "work_mode");
    if (work_mode_item == NULL) {
        return CMD_ABSENT;
    }
    if (!cJSON_IsString(work_mode_item) || work_mode_item->valuestring == NULL) {
        *error = "invalid work_mode";
        return CMD_FAILED;
    }

    const cJSON *task_uuid_item = cJSON_GetObjectItemCaseSensitive((cJSON *)message, "task_uuid");
    const char *task_uuid = (cJSON_IsString(task_uuid_item) && task_uuid_item->valuestring != NULL)
                                ? task_uuid_item->valuestring
                                : "<none>";
    const char *work_mode = work_mode_item->valuestring;

    ESP_LOGI(TAG, "WS command: work_mode=%s task_uuid=%s", work_mode, task_uuid);

    if (strcmp(work_mode, "heater") != 0 && strcmp(work_mode, "air_purifier") != 0) {
        ESP_LOGW(TAG, "Unsupported work_mode command: %s", work_mode);
        *error = "unsupported work_mode";
        return CMD_FAILED;
    }

    esp_err_t err = state_service_set_work_mode(work_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to apply work_mode=%s command: %s",
                 work_mode,
                 esp_err_to_name(err));
        *error = "work_mode not applied";
        return CMD_FAILED;
    }
    return CMD_APPLIED;
}

static cmd_result_t apply_backend_monitor_logging_command(const cJSON *message, const char **error)
{
    if (message == NULL) {
        return CMD_ABSENT;
    }

    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive((cJSON *)message, "monitor_logging_enabled");
    if (enabled_item == NULL) {
        return CMD_ABSENT;
    }
    if (!cJSON_IsBool(enabled_item)) {
        *error = "invalid monitor_logging_enabled";
        return CMD_FAILED;
    }

    const cJSON *task_uuid_item = cJSON_GetObjectItemCaseSensitive((cJSON *)message, "task_uuid");
    const char *task_uuid = (cJSON_IsString(task_uuid_item) && task_uuid_item->valuestring != NULL)
                                ? task_uuid_item->valuestring
                                : "<none>";
    bool enabled = cJSON_IsTrue(enabled_item);

    ESP_LOGW(TAG, "WS command: monitor_logging_enabled=%s task_uuid=%s", enabled ? "true" : "false", task_uuid);
    monitor_logging_service_set_enabled(enabled);
    return CMD_APPLIED;
}

static bool ws_wifi_connected_fast(void)
{
    system_status_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    system_status_get_snapshot(&snapshot);
    if (strncmp(snapshot.wifi_state, "connected", sizeof(snapshot.wifi_state)) == 0) {
        return true;
    }

    state_t state_snapshot;
    if (state_service_get_snapshot(&state_snapshot) == ESP_OK &&
        state_snapshot.wifi_connected_ssid[0] != '\0') {
        ESP_LOGD(TAG, "WS network gate fallback: connected SSID is set despite wifi_state mismatch");
        return true;
    }

    return false;
}

static esp_err_t ws_create_and_start_client(void)
{
    if (s_ws_client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ws_wifi_connected_fast()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t headers_err = build_ws_headers(s_ws_headers, sizeof(s_ws_headers));
    if (headers_err != ESP_OK) {
        return headers_err;
    }

    ESP_LOGI(TAG, "WebSocket connect URL: %s", HEATBIT_API_WS_URL);

    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    const uint32_t stack_candidates[] = {
        WS_TASK_STACK_SIZE,
        WS_TASK_STACK_FALLBACK_1,
        WS_TASK_STACK_FALLBACK_2,
    };
    uint32_t selected_stack = 0;
    for (size_t i = 0; i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
        uint32_t candidate = stack_candidates[i];
        size_t required = (size_t)candidate + (size_t)WS_TASK_INTERNAL_HEADROOM;
        if (internal_largest >= required) {
            selected_stack = candidate;
            break;
        }
    }

    if (selected_stack == 0) {
        size_t min_required = (size_t)WS_TASK_STACK_FALLBACK_2 + (size_t)WS_TASK_INTERNAL_HEADROOM;
        ESP_LOGW(TAG,
                 "WS start deferred: internal heap too low for ws task (required>=%u, largest=%lu, free=%lu)",
                 (unsigned)min_required,
                 (unsigned long)internal_largest,
                 (unsigned long)internal_free);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "WS task stack selected=%u (internal_largest=%lu, internal_free=%lu)",
             (unsigned)selected_stack,
             (unsigned long)internal_largest,
             (unsigned long)internal_free);

    esp_websocket_client_config_t ws_cfg = {
        .uri = HEATBIT_API_WS_URL,
        .headers = s_ws_headers,
        .task_name = "ws_backend_task",
        .task_stack = selected_stack,
        .reconnect_timeout_ms = WS_RECONNECT_MIN_MS,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
    };
    backend_auth_ws(&ws_cfg);

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_websocket_register_events(client,
                                  WEBSOCKET_EVENT_ANY,
                                  ws_event_handler,
                                  NULL);

    /* Publish before start: the CONNECTED handler flushes pending status through s_ws_client. */
    ws_lock();
    s_ws_client = client;
    ws_unlock();

    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        size_t total_free = esp_get_free_heap_size();
        size_t total_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG,
                 "WebSocket start failed: %s stack=%u free_heap=%lu largest_block=%lu internal_free=%lu internal_largest=%lu",
                 esp_err_to_name(err),
                 (unsigned)selected_stack,
                 (unsigned long)total_free,
                 (unsigned long)total_largest,
                 (unsigned long)internal_free,
                 (unsigned long)internal_largest);
        ws_destroy_client(ws_detach_client());
        return err;
    }

    return ESP_OK;
}

static esp_err_t ws_force_reconnect(void)
{
    if (!ws_wifi_connected_fast()) {
        return ESP_ERR_INVALID_STATE;
    }

    ws_destroy_client(ws_detach_client());

    s_ws_connected = false;
    s_rx_collecting_text = false;
    s_rx_text_len = 0;
    return ws_create_and_start_client();
}

static void log_server_close_frame(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
        ESP_LOGW(TAG, "Server initiated CLOSE frame (no payload)");
        return;
    }

    if (data->payload_offset != 0) {
        ESP_LOGW(TAG,
                 "Server CLOSE frame continuation: offset=%d len=%d total=%d",
                 data->payload_offset,
                 data->data_len,
                 data->payload_len);
        return;
    }

    if (data->data_len < 2) {
        ESP_LOGW(TAG, "Server CLOSE frame too short: len=%d", data->data_len);
        return;
    }

    const uint8_t *p = (const uint8_t *)data->data_ptr;
    uint16_t close_code = (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);

    char reason[96] = {0};
    int reason_len = data->data_len - 2;
    if (reason_len > 0) {
        if (reason_len > (int)sizeof(reason) - 1) {
            reason_len = (int)sizeof(reason) - 1;
        }
        memcpy(reason, data->data_ptr + 2, (size_t)reason_len);
        reason[reason_len] = '\0';
    }

    if (reason[0] != '\0') {
        ESP_LOGW(TAG,
                 "Server initiated CLOSE frame: code=%u reason=%s",
                 (unsigned)close_code,
                 reason);
    } else {
        ESP_LOGW(TAG,
                 "Server initiated CLOSE frame: code=%u",
                 (unsigned)close_code);
    }
}

static void ws_status_log_task(void *arg)
{
    (void)arg;

    while (s_ws_started) {
        int64_t now_us = esp_timer_get_time();
        bool wifi_connected = ws_wifi_connected_fast();

        /* Do not keep websocket client alive while Wi-Fi is down. */
        if (!wifi_connected && s_ws_client != NULL) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, suspending websocket client");
            ws_destroy_client(ws_detach_client());
            s_ws_connected = false;
            s_last_disconnect_us = now_us;
        }

        if (s_ws_started && wifi_connected && s_ws_client == NULL) {
            int64_t since_last_reconnect_ms = -1;
            if (s_last_reconnect_attempt_us > 0 && now_us >= s_last_reconnect_attempt_us) {
                since_last_reconnect_ms = (now_us - s_last_reconnect_attempt_us) / 1000;
            }

            bool retry_allowed = (s_last_reconnect_attempt_us == 0) ||
                                 (since_last_reconnect_ms >= (int64_t)s_recreate_gap_ms);

            if (retry_allowed) {
                s_last_reconnect_attempt_us = now_us;
                s_recreate_gap_ms = backoff_next_ms(&s_reconnect_backoff);
                ESP_LOGW(TAG, "WS reconnect watchdog: creating client after disconnect");
                esp_err_t rec_err = ws_force_reconnect();
                if (rec_err != ESP_OK) {
                    ESP_LOGW(TAG, "WS reconnect watchdog: restart failed: %s", esp_err_to_name(rec_err));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    s_status_log_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t send_text_message(const char *message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Checked before taking the lock: it reads state_service, whose setters publish through this module. */
    if (!ws_wifi_connected_fast()) {
        return ESP_ERR_INVALID_STATE;
    }

    ws_lock();
    if (s_ws_client == NULL || !esp_websocket_client_is_connected(s_ws_client)) {
        ws_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "WS TX message: %s", message);

    int sent = esp_websocket_client_send_text(s_ws_client, message, (int)strlen(message), pdMS_TO_TICKS(5000));
    ws_unlock();
    if (sent < 0) {
        ESP_LOGW(TAG, "WS TX send timed out or failed: sent=%d", sent);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WS TX sent bytes=%d", sent);
    s_last_tx_us = esp_timer_get_time();

    return ESP_OK;
}

/* Status buffers are shared by publishers and the CONNECTED flush: callers hold the WS lock. */
static void queue_pending_status(ws_status_message_kind_t kind, const char *message)
{
    if (kind >= WS_STATUS_MESSAGE_COUNT || message == NULL) {
        return;
    }

    if (s_pending_status_valid[kind] &&
        strncmp(s_pending_status[kind], message, sizeof(s_pending_status[kind])) == 0) {
        return;
    }

    strlcpy(s_pending_status[kind], message, sizeof(s_pending_status[kind]));
    s_pending_status_valid[kind] = true;
}

static void flush_pending_status(void)
{
    ws_lock();
    for (size_t i = 0; i < WS_STATUS_MESSAGE_COUNT; i++) {
        if (!s_pending_status_valid[i]) {
            continue;
        }

        if (send_text_message(s_pending_status[i]) == ESP_OK) {
            strlcpy(s_last_status[i], s_pending_status[i], sizeof(s_last_status[i]));
            s_last_status_valid[i] = true;
            s_pending_status_valid[i] = false;
            continue;
        }

        break;
    }
    ws_unlock();
}

static esp_err_t publish_status_message(ws_status_message_kind_t kind, cJSON *root)
{
    if (kind >= WS_STATUS_MESSAGE_COUNT || root == NULL) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return ESP_ERR_INVALID_ARG;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Publishing backend status[%u]: %s", (unsigned)kind, json);
    ws_lock();
    esp_err_t err = send_text_message(json);
    if (err == ESP_ERR_INVALID_STATE) {
        queue_pending_status(kind, json);
        err = ESP_OK;
    } else if (err == ESP_OK) {
        strlcpy(s_last_status[kind], json, sizeof(s_last_status[kind]));
        s_last_status_valid[kind] = true;
    }
    ws_unlock();
    cJSON_free(json);

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to send backend status[%u] over websocket: %s",
                 (unsigned)kind,
                 esp_err_to_name(err));
    }

    return err;
}

static esp_err_t build_ws_headers(char *headers, size_t headers_size)
{
    if (headers == NULL || headers_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    system_status_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    system_status_get_snapshot(&snapshot);

    if (snapshot.device_id[0] == '\0') {
        ESP_LOGE(TAG, "Cannot build WS headers: missing device_id");
        return ESP_ERR_INVALID_STATE;
    }

    /* Authentication headers come first, then the device identification. */
    esp_err_t err = backend_auth_ws_headers(headers, headers_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS auth headers failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t used = strlen(headers);
    int written = snprintf(headers + used,
                           headers_size - used,
                           "Miner: %s\r\n"
                           "Model: %s\r\n",
                           snapshot.device_id,
                           HEATBIT_DEVICE_MODEL_BACKEND);
    if (written <= 0 || (size_t)written >= headers_size - used) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void handle_backend_message(const cJSON *message)
{
    if (message == NULL || !cJSON_IsObject((cJSON *)message)) {
        ESP_LOGD(TAG, "Backend message is not a valid JSON object");
        return;
    }

    char *json = cJSON_PrintUnformatted((cJSON *)message);
    if (json == NULL) {
        ESP_LOGI(TAG, "Backend message received (failed to stringify JSON)");
        return;
    }

    ESP_LOGI(TAG, "Backend message: %s", json);
    cJSON_free(json);

    /* Run the commands first, then acknowledge with the actual result. */
    const char *error = NULL;
    bool any = false;
    bool failed = false;
    cmd_result_t results[3];
    /* Same order as before: work mode, fan mode (which also sets power), logging. */
    results[0] = apply_backend_work_mode_command(message, &error);
    results[1] = apply_backend_fan_mode_command(message, &error);
    results[2] = apply_backend_monitor_logging_command(message, &error);
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        any |= results[i] != CMD_ABSENT;
        failed |= results[i] == CMD_FAILED;
    }
    if (!any) {
        error = "unsupported command";
    }
    send_backend_command_ack(message, any && !failed, error);
}

static void on_message(const char *message_str)
{
    s_received_at_us = esp_timer_get_time();

    ESP_LOGI(TAG, "WS RX message: %s", message_str != NULL ? message_str : "<null>");

    if (message_str == NULL) {
        ESP_LOGW(TAG, "WS RX message is null");
        return;
    }

    cJSON *message = cJSON_Parse(message_str);
    if (message == NULL) {
        ESP_LOGW(TAG, "Fail to convert message to dictionary");
        return;
    }

    handle_backend_message(message);
    cJSON_Delete(message);
}

static void ws_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (data == NULL) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_connected = true;
        s_last_disconnect_us = 0;
        backoff_reset(&s_reconnect_backoff);
        s_recreate_gap_ms = 0;
        (void)esp_websocket_client_set_reconnect_timeout(data->client, WS_RECONNECT_MIN_MS);
        ESP_LOGI(TAG, "WebSocket connected to backend");
        flush_pending_status();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED: {
        s_ws_connected = false;
        s_last_disconnect_us = esp_timer_get_time();
        uint32_t delay_ms = backoff_next_ms(&s_reconnect_backoff);
        (void)esp_websocket_client_set_reconnect_timeout(data->client, (int)delay_ms);
        ESP_LOGW(TAG, "WebSocket disconnected, reconnect in %lu ms", (unsigned long)delay_ms);
        break;
    }
    case WEBSOCKET_EVENT_DATA: {
        if (data->data_len <= 0 || data->data_ptr == NULL) {
            break;
        }

        if (data->op_code == WS_OPCODE_CLOSE) {
            s_ws_connected = false;
            s_last_disconnect_us = esp_timer_get_time();
            log_server_close_frame(data);
            break;
        }

        if (data->payload_offset == 0) {
            s_rx_text_len = 0;
            s_rx_collecting_text = (data->op_code == WS_OPCODE_TEXT);

            if (!s_rx_collecting_text) {
                ESP_LOGW(TAG,
                         "Ignoring non-text WS frame: opcode=%d payload_len=%d",
                         data->op_code,
                         data->payload_len);
            }
        } else if (data->op_code != WS_OPCODE_CONTINUATION) {
            /* Mid-message non-continuation frame; reset assembler defensively. */
            s_rx_collecting_text = false;
            s_rx_text_len = 0;
            ESP_LOGW(TAG,
                     "Unexpected fragmented WS frame opcode=%d offset=%d len=%d",
                     data->op_code,
                     data->payload_offset,
                     data->data_len);
            break;
        }

        if (!s_rx_collecting_text) {
            break;
        }

        if ((size_t)data->payload_offset != s_rx_text_len) {
            s_rx_collecting_text = false;
            s_rx_text_len = 0;
            ESP_LOGW(TAG,
                     "WS text fragment offset mismatch: expected=%u got=%d",
                     (unsigned)s_rx_text_len,
                     data->payload_offset);
            break;
        }

        size_t copy_len = (size_t)data->data_len;
        if (copy_len > sizeof(s_rx_text_buf) - 1 - s_rx_text_len) {
            s_rx_collecting_text = false;
            s_rx_text_len = 0;
            ESP_LOGW(TAG,
                     "WS text frame too large: payload_len=%d capacity=%u",
                     data->payload_len,
                     (unsigned)sizeof(s_rx_text_buf));
            break;
        }

        memcpy(&s_rx_text_buf[s_rx_text_len], data->data_ptr, copy_len);
        s_rx_text_len += copy_len;

        bool is_final_fragment = ((size_t)(data->payload_offset + data->data_len) >= (size_t)data->payload_len);
        if (is_final_fragment) {
            s_rx_text_buf[s_rx_text_len] = '\0';
            on_message(s_rx_text_buf);
            s_rx_text_len = 0;
            s_rx_collecting_text = false;
        }
        break;
    }
    case WEBSOCKET_EVENT_CLOSED:
        s_ws_connected = false;
        s_last_disconnect_us = esp_timer_get_time();
        ESP_LOGW(TAG, "WebSocket closed cleanly by peer");
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_ws_connected = false;
        s_last_disconnect_us = esp_timer_get_time();
        ESP_LOGW(TAG,
                 "WebSocket error event: type=%d tls_err=%s sock_errno=%d hs_status=%d free_heap=%lu largest_block=%lu",
                 data->error_handle.error_type,
                 esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                 data->error_handle.esp_transport_sock_errno,
                 data->error_handle.esp_ws_handshake_status_code,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        break;
    default:
        break;
    }
}

/* Overall air quality from TVOC (raw) and CO2 (ppm): the worse of the two. */
static const char *get_air_quality_value(int tvoc_raw, int co2_ppm) {
    int tvoc_level;
    if (tvoc_raw < 300) tvoc_level = 0;
    else if (tvoc_raw <= 600) tvoc_level = 1;
    else if (tvoc_raw <= 1200) tvoc_level = 2;
    else tvoc_level = 3;

    int co2_level;
    if (co2_ppm < 600) co2_level = 0;
    else if (co2_ppm <= 1000) co2_level = 1;
    else if (co2_ppm <= 1500) co2_level = 2;
    else co2_level = 3;

    int overall = (tvoc_level > co2_level) ? tvoc_level : co2_level;

    switch (overall) {
        case 0: return "good";
        case 1: return "moderate";
        case 2: return "fair";
        case 3: return "poor";
        default: return "--";
    }
}

static void air_quality_level_to_color(const char *level, uint8_t rgb[3]) {
    if (strcmp(level, "good") == 0) {
        rgb[0] = 0;
        rgb[1] = 178;
        rgb[2] = 62;
    }
    else if (strcmp(level, "moderate") == 0) {
        rgb[0] = 253;
        rgb[1] = 171;
        rgb[2] = 8;
    }
    else if (strcmp(level, "fair") == 0) {
        rgb[0] = 241;
        rgb[1] = 90;
        rgb[2] = 36;
    }
    else if (strcmp(level, "poor") == 0) {
        rgb[0] = 223;
        rgb[1] = 26;
        rgb[2] = 26;
    }
    else {
        // Unknown level: white
        rgb[0] = 255;
        rgb[1] = 255;
        rgb[2] = 255;
    }
}

esp_err_t ws_backend_service_start(void)
{
    if (s_ws_started) {
        return ESP_OK;
    }

    s_ws_started = true;
    s_ws_connected = false;
    s_rx_collecting_text = false;
    s_rx_text_len = 0;
    s_last_disconnect_us = esp_timer_get_time();
    s_last_reconnect_attempt_us = 0;

    esp_err_t err = ws_create_and_start_client();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS start deferred until Wi-Fi is connected: %s", esp_err_to_name(err));
    }

    if (s_status_log_task == NULL) {
        BaseType_t created = xTaskCreate(ws_status_log_task,
                                         "ws_status_log",
                                         3072,
                                         NULL,
                                         tskIDLE_PRIORITY + 1,
                                         &s_status_log_task);
        if (created != pdPASS) {
            s_status_log_task = NULL;
            ESP_LOGW(TAG, "Failed to start WS status log task");
        }
    }

    ESP_LOGI(TAG, "WebSocket backend service started");
    return ESP_OK;
}

esp_err_t ws_backend_service_stop(void)
{
    if (!s_ws_started) {
        return ESP_OK;
    }

    /* Clearing s_ws_started also ends the reconnect watchdog. */
    s_ws_started = false;
    s_ws_connected = false;
    ws_destroy_client(ws_detach_client());
    return ESP_OK;
}

esp_err_t ws_backend_service_publish_status(const char *work_mode,
                                            const char *fan_mode,
                                            bool power_pin,
                                            const char *version,
                                            uint32_t mhs_5s,
                                            float miner_consumption,
                                            float voc,
                                            float gas,
                                            float co2)

{
    (void)version;

    if (work_mode == NULL || fan_mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char miner_id[STATE_UUID_LEN] = {0};
    state_t snapshot;
    if (state_service_get_snapshot(&snapshot) == ESP_OK) {
        strlcpy(miner_id, snapshot.device_id, sizeof(miner_id));
    }

    cJSON *air_quality_msg = cJSON_CreateObject();
    if (air_quality_msg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(air_quality_msg, "voc", voc/1000);
    cJSON_AddNumberToObject(air_quality_msg, "co2", co2);
    cJSON_AddNumberToObject(air_quality_msg, "gas", gas);
    const char *air_quality_status_value = get_air_quality_value(voc, co2);
    cJSON_AddStringToObject(air_quality_msg, "air_quality_status", air_quality_status_value);
    cJSON *color_array = cJSON_AddArrayToObject(air_quality_msg, "color");

    uint8_t color[3];
    air_quality_level_to_color(air_quality_status_value, color);

    cJSON_AddItemToArray(color_array, cJSON_CreateNumber(color[0]));
    cJSON_AddItemToArray(color_array, cJSON_CreateNumber(color[1]));
    cJSON_AddItemToArray(color_array, cJSON_CreateNumber(color[2]));

    cJSON_AddStringToObject(air_quality_msg, "miner_id", miner_id);

    esp_err_t err = publish_status_message(WS_STATUS_MESSAGE_AIR_QUALITY, air_quality_msg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *miner_msg = cJSON_CreateObject();
    if (miner_msg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(miner_msg, "miner_consumption", miner_consumption);
    cJSON_AddNumberToObject(miner_msg, "heater_consumption", miner_consumption);
    cJSON_AddNumberToObject(miner_msg, "mhs_5s", (double)mhs_5s);
    cJSON_AddStringToObject(miner_msg, "miner_id", miner_id);

    err = publish_status_message(WS_STATUS_MESSAGE_MINER, miner_msg);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *power_msg = cJSON_CreateObject();
    if (power_msg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(power_msg, "work_mode", work_mode);
    cJSON_AddStringToObject(power_msg, "fan_mode", fan_mode);
    cJSON_AddBoolToObject(power_msg, "power_pin", power_pin);
    cJSON_AddNumberToObject(power_msg, "power_mode", power_pin ? 1 : 0);
    cJSON_AddStringToObject(power_msg, "miner_id", miner_id);

    return publish_status_message(WS_STATUS_MESSAGE_POWER, power_msg);
}

void ws_backend_service_state_listener(state_change_t change, const state_t *snapshot)
{
    if (change == STATE_CHANGE_NETWORK || snapshot == NULL) {
        return;
    }
    ws_backend_service_publish_status(snapshot->work_mode,
                                      snapshot->fan_mode,
                                      snapshot->power_pin,
                                      firmware_version_get_version(),
                                      snapshot->mhs_5s,
                                      snapshot->miner_consumption,
                                      snapshot->sensor_data.voc,
                                      snapshot->sensor_data.gas,
                                      snapshot->sensor_data.co2);
}

esp_err_t ws_backend_service_get_runtime_status(bool *out_started,
                                                bool *out_connected,
                                                int64_t *out_last_tx_us,
                                                int64_t *out_last_rx_us)
{
    if (out_started == NULL || out_connected == NULL || out_last_tx_us == NULL || out_last_rx_us == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_started = s_ws_started;
    *out_connected = s_ws_connected;
    *out_last_tx_us = s_last_tx_us;
    *out_last_rx_us = s_received_at_us;
    return ESP_OK;
}
