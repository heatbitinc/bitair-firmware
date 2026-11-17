#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum string lengths */
#define STATE_UUID_LEN 37
#define STATE_WORKER_ID_LEN 15
#define STATE_SSID_LEN 33
#define STATE_PASSWORD_LEN 65
#define STATE_FAN_MODE_LEN 16
#define STATE_WORK_MODE_LEN 16

typedef enum {
    STATE_FAN_MODE_SOURCE_UNKNOWN = 0,
    STATE_FAN_MODE_SOURCE_PWM_MONITOR,
    STATE_FAN_MODE_SOURCE_REMOTE_CONTROL,
} state_fan_mode_source_t;

/* Sensor data */
typedef struct {
    float voc;
    float gas;
    float co2;
    float temp;
    float humidity;
} state_sensor_data_t;

/* Complete device state */
typedef struct {
    /* Device IDs */
    char device_id[STATE_UUID_LEN];      /* UUID */
    char worker_id[STATE_WORKER_ID_LEN]; /* Written at manufacturing */

    /* Wi-Fi settings */
    char wifi_ssid[STATE_SSID_LEN];
    char wifi_password[STATE_PASSWORD_LEN];
    char wifi_connected_ssid[STATE_SSID_LEN]; /* Current connected SSID */

    /* Metrics data */
    uint32_t mhs_5s;
    char fan_mode[STATE_FAN_MODE_LEN];
    char work_mode[STATE_WORK_MODE_LEN];
    bool power_pin;
    state_sensor_data_t sensor_data;
    float miner_consumption;
} state_t;

/* Initialization and persistence */
esp_err_t state_service_init(void);
esp_err_t state_service_load(void);
esp_err_t state_service_save(void);

/* Device ID getters/setters */
esp_err_t state_service_get_device_id(char out_uuid[STATE_UUID_LEN]);
esp_err_t state_service_set_device_id(const char *uuid);
esp_err_t state_service_get_worker_id(char out_worker_id[STATE_WORKER_ID_LEN]);
esp_err_t state_service_set_worker_id(const char *worker_id);

/* Wi-Fi settings getters/setters */
esp_err_t state_service_get_wifi_ssid(char out_ssid[STATE_SSID_LEN]);
esp_err_t state_service_set_wifi_ssid(const char *ssid);
esp_err_t state_service_get_wifi_password(char out_password[STATE_PASSWORD_LEN]);
esp_err_t state_service_set_wifi_password(const char *password);

/* Connected Wi-Fi SSID (runtime, not persisted) */
esp_err_t state_service_get_connected_wifi_ssid(char out_ssid[STATE_SSID_LEN]);
esp_err_t state_service_set_connected_wifi_ssid(const char *ssid);

/* Metrics getters/setters */
esp_err_t state_service_get_mhs_5s(uint32_t *out_mhs_5s);
esp_err_t state_service_set_mhs_5s(uint32_t mhs_5s);

esp_err_t state_service_get_fan_mode(char out_mode[STATE_FAN_MODE_LEN]);
esp_err_t state_service_set_fan_mode(const char *mode);
esp_err_t state_service_set_fan_mode_with_source(const char *mode, state_fan_mode_source_t source);
esp_err_t state_service_get_fan_mode_source(state_fan_mode_source_t *out_source);

esp_err_t state_service_get_work_mode(char out_mode[STATE_WORK_MODE_LEN]);
esp_err_t state_service_set_work_mode(const char *mode);

esp_err_t state_service_get_power_pin(bool *out_power_pin);
esp_err_t state_service_set_power_pin(bool power_pin);

esp_err_t state_service_get_miner_consumption(float *out_miner_consumption);
esp_err_t state_service_set_miner_consumption(float miner_consumption);

esp_err_t state_service_get_sensor_data(state_sensor_data_t *out_sensor_data);
esp_err_t state_service_set_sensor_data(const state_sensor_data_t *sensor_data);
/* Update only the fields one sensor owns, so sensors do not overwrite each other. */
esp_err_t state_service_set_air_quality(float voc, float gas, float co2);
esp_err_t state_service_set_climate(float temp, float humidity);

/* Snapshot (get entire state) */
esp_err_t state_service_get_snapshot(state_t *out_snapshot);

/*
 * Change notifications. The state service only stores state; whoever reacts to
 * a change (LED, mining gate, backend status) registers a listener. Listeners
 * run in the task that made the change, after the state lock is released, in
 * registration order.
 */
typedef enum {
    STATE_CHANGE_POWER_PIN,
    STATE_CHANGE_FAN_MODE,
    STATE_CHANGE_WORK_MODE,
    STATE_CHANGE_NETWORK, /* connectivity changed; the state itself did not */
} state_change_t;

typedef void (*state_service_listener_t)(state_change_t change, const state_t *snapshot);

esp_err_t state_service_add_listener(state_service_listener_t listener);

/* Tells listeners that network connectivity changed (e.g. to update the LED). */
esp_err_t state_service_notify_network_changed(void);

#ifdef __cplusplus
}
#endif
