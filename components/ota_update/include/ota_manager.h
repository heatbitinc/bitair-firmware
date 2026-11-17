#pragma once

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    const char * server_url;
    const char * version_path;   /* appended to server_url: returns the latest version */
    const char * download_path;  /* appended to server_url: returns the latest image */
    const char * device_model;
    /* Sets TLS and client authentication on every OTA request; CA bundle only if NULL. */
    void (*configure_http)(esp_http_client_config_t * config);
    uint32_t check_interval_s;
    uint32_t task_stack_size;
    uint32_t task_priority;
} ota_config_t;

ESP_EVENT_DECLARE_BASE(OTA_EVENTS);

typedef enum
{
    OTA_EVENT_UPDATE_FOUND,
    OTA_EVENT_START_DOWNLOAD,
    OTA_EVENT_FINISH_DOWNLOAD,
    OTA_EVENT_CHECK_FW_FAILED,
    OTA_EVENT_READY_FOR_REBOOT
} ota_event_id_t;

typedef struct
{
    char version[32];
} ota_event_data_t;

typedef enum {
    OTA_CHECK_STATUS_UNKNOWN = 0,
    OTA_CHECK_STATUS_OK,
    OTA_CHECK_STATUS_FAILED,
} ota_check_status_t;

typedef enum {
    OTA_PROCESS_STATUS_IDLE = 0,
    OTA_PROCESS_STATUS_DOWNLOADING,
    OTA_PROCESS_STATUS_READY_FOR_REBOOT,
    OTA_PROCESS_STATUS_FAILED,
} ota_process_status_t;

typedef struct {
    int64_t last_check_unix_s;
    ota_check_status_t check_status;
    bool update_needed;
    bool update_needed_known;
    ota_process_status_t process_status;
    char update_version[32];
} ota_runtime_status_t;

esp_err_t ota_manager_start(const ota_config_t * config);
esp_err_t ota_manager_trigger_check(void);
esp_err_t ota_manager_get_runtime_status(ota_runtime_status_t *out_status);
