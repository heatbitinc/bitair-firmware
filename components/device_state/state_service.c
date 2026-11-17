#include <math.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "state_service.h"

static const char *TAG = "state_service";

static const char *NVS_NAMESPACE = "device_state";
static const char *NVS_KEY_DEVICE_ID = "device_id";
static const char *NVS_KEY_WORKER_ID = "worker_id";
static const char *NVS_KEY_WIFI_SSID = "wifi_ssid";
static const char *NVS_KEY_WIFI_PASS = "wifi_pass";
static const char *NVS_KEY_MHS_5S = "mhs_5s";
static const char *NVS_KEY_FAN_MODE = "fan_mode";
static const char *NVS_KEY_WORK_MODE = "work_mode";
static const char *NVS_KEY_POWER_PIN = "power_pin";
static const char *NVS_KEY_MINER_CONSUMPTION = "miner_consumption";

static state_t s_state;
static SemaphoreHandle_t s_state_mutex;
static bool s_initialized;
static state_fan_mode_source_t s_fan_mode_source = STATE_FAN_MODE_SOURCE_UNKNOWN;

static bool is_valid_fan_mode(const char *mode)
{
    if (mode == NULL) {
        return false;
    }

    return strcmp(mode, "off") == 0 || strcmp(mode, "silent") == 0 || strcmp(mode, "auto") == 0
           || strcmp(mode, "turbo") == 0;
}

#define STATE_LISTENERS_MAX 8
static state_service_listener_t s_listeners[STATE_LISTENERS_MAX];
static size_t s_listener_count;

static void notify_listeners(state_change_t change)
{
    state_t snapshot;
    if (state_service_get_snapshot(&snapshot) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < s_listener_count; i++) {
        s_listeners[i](change, &snapshot);
    }
}

static const char *fan_mode_source_to_str(state_fan_mode_source_t source)
{
    switch (source) {
    case STATE_FAN_MODE_SOURCE_PWM_MONITOR:
        return "pwm_monitor";
    case STATE_FAN_MODE_SOURCE_REMOTE_CONTROL:
        return "remote_control";
    case STATE_FAN_MODE_SOURCE_UNKNOWN:
    default:
        return "unknown";
    }
}

esp_err_t state_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_state, 0, sizeof(s_state));

    /* Set defaults */
    strlcpy(s_state.fan_mode, "auto", sizeof(s_state.fan_mode));
    s_fan_mode_source = STATE_FAN_MODE_SOURCE_PWM_MONITOR;
    strlcpy(s_state.work_mode, "heater", sizeof(s_state.work_mode));

    s_initialized = true;
    ESP_LOGI(TAG, "State service initialized");
    return ESP_OK;
}

esp_err_t state_service_load(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist yet, that's okay */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        nvs_close(nvs);
        return ESP_ERR_TIMEOUT;
    }

    /* Load strings */
    size_t len;

    len = STATE_UUID_LEN;
    nvs_get_str(nvs, NVS_KEY_DEVICE_ID, s_state.device_id, &len);

    len = STATE_WORKER_ID_LEN;
    nvs_get_str(nvs, NVS_KEY_WORKER_ID, s_state.worker_id, &len);

    len = STATE_SSID_LEN;
    nvs_get_str(nvs, NVS_KEY_WIFI_SSID, s_state.wifi_ssid, &len);

    len = STATE_PASSWORD_LEN;
    nvs_get_str(nvs, NVS_KEY_WIFI_PASS, s_state.wifi_password, &len);

    len = STATE_FAN_MODE_LEN;
    nvs_get_str(nvs, NVS_KEY_FAN_MODE, s_state.fan_mode, &len);

    /* work_mode is saved but intentionally not restored: the device starts in heater mode. */

    /* Load numeric values */
    nvs_get_u32(nvs, NVS_KEY_MHS_5S, &s_state.mhs_5s);

    uint32_t miner_i32 = 0;
    if (nvs_get_i32(nvs, NVS_KEY_MINER_CONSUMPTION, (int32_t *)&miner_i32) == ESP_OK) {
        memcpy(&s_state.miner_consumption, &miner_i32, sizeof(float));
    }

    uint8_t bool_val = 0;
    if (nvs_get_u8(nvs, NVS_KEY_POWER_PIN, &bool_val) == ESP_OK) {
        s_state.power_pin = (bool)bool_val;
    }

    xSemaphoreGive(s_state_mutex);
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t state_service_save(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        nvs_close(nvs);
        return ESP_ERR_TIMEOUT;
    }

    /* Save strings */
    nvs_set_str(nvs, NVS_KEY_DEVICE_ID, s_state.device_id);
    nvs_set_str(nvs, NVS_KEY_WORKER_ID, s_state.worker_id);
    nvs_set_str(nvs, NVS_KEY_WIFI_SSID, s_state.wifi_ssid);
    nvs_set_str(nvs, NVS_KEY_WIFI_PASS, s_state.wifi_password);
    nvs_set_str(nvs, NVS_KEY_FAN_MODE, s_state.fan_mode);
    nvs_set_str(nvs, NVS_KEY_WORK_MODE, s_state.work_mode);

    /* Save numeric values */
    nvs_set_u32(nvs, NVS_KEY_MHS_5S, s_state.mhs_5s);

    uint32_t heater_i32 = 0;
    memcpy(&heater_i32, &s_state.miner_consumption, sizeof(float));
    nvs_set_i32(nvs, NVS_KEY_MINER_CONSUMPTION, (int32_t)heater_i32);

    err = nvs_commit(nvs);
    xSemaphoreGive(s_state_mutex);
    nvs_close(nvs);
    return err;
}

/* Device ID getters/setters */
esp_err_t state_service_get_device_id(char out_uuid[STATE_UUID_LEN])
{
    if (out_uuid == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(out_uuid, s_state.device_id, STATE_UUID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_device_id(const char *uuid)
{
    if (uuid == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(s_state.device_id, uuid, STATE_UUID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_get_worker_id(char out_worker_id[STATE_WORKER_ID_LEN])
{
    if (out_worker_id == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(out_worker_id, s_state.worker_id, STATE_WORKER_ID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_worker_id(const char *worker_id)
{
    if (worker_id == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(s_state.worker_id, worker_id, STATE_WORKER_ID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

/* Wi-Fi settings getters/setters */
esp_err_t state_service_get_wifi_ssid(char out_ssid[STATE_SSID_LEN])
{
    if (out_ssid == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(out_ssid, s_state.wifi_ssid, STATE_SSID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_wifi_ssid(const char *ssid)
{
    if (ssid == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(s_state.wifi_ssid, ssid, STATE_SSID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_get_wifi_password(char out_password[STATE_PASSWORD_LEN])
{
    if (out_password == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(out_password, s_state.wifi_password, STATE_PASSWORD_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_wifi_password(const char *password)
{
    if (password == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(s_state.wifi_password, password, STATE_PASSWORD_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

/* Connected Wi-Fi SSID (runtime only, not persisted) */
esp_err_t state_service_get_connected_wifi_ssid(char out_ssid[STATE_SSID_LEN])
{
    if (out_ssid == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(out_ssid, s_state.wifi_connected_ssid, STATE_SSID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_connected_wifi_ssid(const char *ssid)
{
    if (ssid == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(s_state.wifi_connected_ssid, ssid, STATE_SSID_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

/* Metrics getters/setters */
esp_err_t state_service_get_mhs_5s(uint32_t *out_mhs_5s)
{
    if (out_mhs_5s == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_mhs_5s = s_state.mhs_5s;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_mhs_5s(uint32_t mhs_5s)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_state.mhs_5s = mhs_5s;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_get_fan_mode(char out_mode[STATE_FAN_MODE_LEN])
{
    if (out_mode == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(out_mode, s_state.fan_mode, STATE_FAN_MODE_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_fan_mode(const char *mode)
{
    return state_service_set_fan_mode_with_source(mode, STATE_FAN_MODE_SOURCE_UNKNOWN);
}

esp_err_t state_service_set_fan_mode_with_source(const char *mode, state_fan_mode_source_t source)
{
    if (!s_initialized || !is_valid_fan_mode(mode)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (source == STATE_FAN_MODE_SOURCE_UNKNOWN) {
        source = (s_fan_mode_source == STATE_FAN_MODE_SOURCE_UNKNOWN) ? STATE_FAN_MODE_SOURCE_PWM_MONITOR
                                                                       : s_fan_mode_source;
    }

    if (strncmp(s_state.fan_mode, mode, STATE_FAN_MODE_LEN) == 0 && s_fan_mode_source == source) {
        xSemaphoreGive(s_state_mutex);
        return ESP_OK;
    }

    char previous_mode[STATE_FAN_MODE_LEN] = {0};
    state_fan_mode_source_t previous_source = s_fan_mode_source;
    strlcpy(previous_mode, s_state.fan_mode, sizeof(previous_mode));
    strlcpy(s_state.fan_mode, mode, STATE_FAN_MODE_LEN);
    s_fan_mode_source = source;
    xSemaphoreGive(s_state_mutex);

    const char *caller_task = pcTaskGetName(NULL);
    ESP_LOGW(TAG,
             "fan_mode changed by %s: %s(%s) -> %s(%s)",
             caller_task != NULL ? caller_task : "<unknown>",
             previous_mode,
             fan_mode_source_to_str(previous_source),
             mode,
             fan_mode_source_to_str(source));

    notify_listeners(STATE_CHANGE_FAN_MODE);
    return ESP_OK;
}

esp_err_t state_service_get_fan_mode_source(state_fan_mode_source_t *out_source)
{
    if (out_source == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_source = s_fan_mode_source;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_get_work_mode(char out_mode[STATE_WORK_MODE_LEN])
{
    if (out_mode == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(out_mode, s_state.work_mode, STATE_WORK_MODE_LEN);
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_work_mode(const char *mode)
{
    if (mode == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (strncmp(s_state.work_mode, mode, STATE_WORK_MODE_LEN) == 0) {
        xSemaphoreGive(s_state_mutex);
        return ESP_OK;
    }

    strlcpy(s_state.work_mode, mode, STATE_WORK_MODE_LEN);
    xSemaphoreGive(s_state_mutex);

    notify_listeners(STATE_CHANGE_WORK_MODE);
    return ESP_OK;
}

esp_err_t state_service_get_power_pin(bool *out_power_pin)
{
    if (out_power_pin == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_power_pin = s_state.power_pin;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_power_pin(bool power_pin)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_state.power_pin == power_pin) {
        xSemaphoreGive(s_state_mutex);
        return ESP_OK;
    }

    bool previous_power_pin = s_state.power_pin;
    s_state.power_pin = power_pin;
    xSemaphoreGive(s_state_mutex);

    const char *caller_task = pcTaskGetName(NULL);
    ESP_LOGW(TAG,
             "power_pin changed by %s: %s -> %s",
             caller_task != NULL ? caller_task : "<unknown>",
             previous_power_pin ? "true" : "false",
             power_pin ? "true" : "false");

    notify_listeners(STATE_CHANGE_POWER_PIN);
    return ESP_OK;
}

esp_err_t state_service_get_miner_consumption(float *out_miner_consumption)
{
    if (out_miner_consumption == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_miner_consumption = s_state.miner_consumption;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_miner_consumption(float miner_consumption)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_state.miner_consumption = miner_consumption;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_get_sensor_data(state_sensor_data_t *out_sensor_data)
{
    if (out_sensor_data == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out_sensor_data, &s_state.sensor_data, sizeof(*out_sensor_data));
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_sensor_data(const state_sensor_data_t *sensor_data)
{
    if (sensor_data == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(&s_state.sensor_data, sensor_data, sizeof(s_state.sensor_data));
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_air_quality(float voc, float gas, float co2)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_state.sensor_data.voc = voc;
    s_state.sensor_data.gas = gas;
    s_state.sensor_data.co2 = co2;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_set_climate(float temp, float humidity)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_state.sensor_data.temp = temp;
    s_state.sensor_data.humidity = humidity;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_get_snapshot(state_t *out_snapshot)
{
    if (out_snapshot == NULL || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out_snapshot, &s_state, sizeof(s_state));
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t state_service_add_listener(state_service_listener_t listener)
{
    if (listener == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_listener_count >= STATE_LISTENERS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    s_listeners[s_listener_count++] = listener;
    return ESP_OK;
}

esp_err_t state_service_notify_network_changed(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    notify_listeners(STATE_CHANGE_NETWORK);
    return ESP_OK;
}
