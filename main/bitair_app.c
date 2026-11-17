/*
 * Bitair application layer: everything app_main does on top of the upstream
 * ESP-Miner startup sequence. app_main calls these hooks at fixed points so the
 * upstream sequence stays readable and close to upstream.
 */
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "aht20_service.h"
#include "asic_reset.h"
#include "asic_result_task.h"
#include "backend_auth.h"
#include "ble_server.h"
#include "bitair_app.h"
#include "create_jobs_task.h"
#include "device_cert.h"
#include "device_id.h"
#include "fan_controller_task.h"
#include "hashrate_monitor_task.h"
#include "indication_service.h"
#include "metrics_service.h"
#include "miner_status_provider.h"
#include "monitor_logging_service.h"
#include "mqtt_service.h"
#include "nvs_config.h"
#include "ota_manager.h"
#include "pool_config_applier.h"
#include "power_management_task.h"
#include "pwm_duty_monitor.h"
#include "self_test.h"
#include "state_listeners.h"
#include "state_service.h"
#include "status_reporter.h"
#include "system.h"
#include "system_status.h"
#include "time_sync_service.h"
#include "tpm300a_service.h"
#include "vcore.h"
#include "wifi_connect.h"
#include "ws_backend_service.h"

#ifndef CONFIG_FORCE_MINING_DISABLED
#define CONFIG_FORCE_MINING_DISABLED 0
#endif

static const char *TAG = "bitair_app";
static GlobalState *s_gs;
static char s_device_uuid[DEVICE_UUID4_STR_LEN];

static const uint32_t PWM_STATE_SYNC_TIMEOUT_MS = 3000;
static const uint32_t OTA_REBOOT_DELAY_MS = 3000;
static const uint32_t OTA_MINING_STOP_TIMEOUT_MS = 15000;
static const uint32_t OTA_MINING_STOP_POLL_MS = 100;
static const uint32_t MINING_WORKER_RETRY_DELAY_MS = 2000;
// mining_start(): VCORE settle, ASIC init (about 13 s on Bitair) and 2 s
// stabilisation, with margin.
static const uint32_t FIRST_MINING_START_TIMEOUT_MS = 60000;
static const uint32_t MINING_WORKER_MAX_RETRIES = 30;

// Boot-time copies for async BLE/Wi-Fi startup tasks.
static char s_ble_boot_device_id[DEVICE_UUID4_STR_LEN] = {0};
static char s_wifi_boot_device_id[DEVICE_UUID4_STR_LEN] = {0};
static TaskHandle_t s_create_jobs_task_handle = NULL;
static TaskHandle_t s_asic_result_task_handle = NULL;


EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static bool s_ota_reboot_scheduled = false;

static bool ensure_create_jobs_task_running(void)
{
    if (s_create_jobs_task_handle != NULL) {
        return true;
    }

    if (xTaskCreateWithCaps(create_jobs_task,
                            "stratum miner",
                            8192,
                            (void *) s_gs,
                            20,
                            &s_create_jobs_task_handle,
                            MALLOC_CAP_SPIRAM) == pdPASS) {
        ESP_LOGI(TAG, "create_jobs_task started (PSRAM stack)");
        return true;
    }

    if (xTaskCreate(create_jobs_task,
                    "stratum miner",
                    7168,
                    (void *) s_gs,
                    20,
                    &s_create_jobs_task_handle) == pdPASS) {
        ESP_LOGI(TAG, "create_jobs_task started (internal RAM stack)");
        return true;
    }

    ESP_LOGE(TAG, "Error creating stratum miner task");
    return false;
}

static bool ensure_asic_result_task_running(void)
{
    if (s_asic_result_task_handle != NULL) {
        return true;
    }

    if (xTaskCreateWithCaps(ASIC_result_task,
                            "asic result",
                            8192,
                            (void *) s_gs,
                            15,
                            &s_asic_result_task_handle,
                            MALLOC_CAP_SPIRAM) == pdPASS) {
        ESP_LOGI(TAG, "ASIC_result_task started (PSRAM stack)");
        return true;
    }

    if (xTaskCreate(ASIC_result_task,
                    "asic result",
                    7168,
                    (void *) s_gs,
                    15,
                    &s_asic_result_task_handle) == pdPASS) {
        ESP_LOGI(TAG, "ASIC_result_task started (internal RAM stack)");
        return true;
    }

    ESP_LOGE(TAG, "Error creating asic result task");
    return false;
}

static void mining_workers_retry_task(void *arg)
{
    (void)arg;

    for (uint32_t attempt = 1; attempt <= MINING_WORKER_MAX_RETRIES; attempt++) {
        bool jobs_ok = ensure_create_jobs_task_running();
        bool result_ok = ensure_asic_result_task_running();
        if (jobs_ok && result_ok) {
            ESP_LOGI(TAG, "Mining worker tasks are running");
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG,
                 "Mining worker task retry %lu/%lu in %lu ms",
                 (unsigned long)attempt,
                 (unsigned long)MINING_WORKER_MAX_RETRIES,
                 (unsigned long)MINING_WORKER_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(MINING_WORKER_RETRY_DELAY_MS));
    }

    ESP_LOGE(TAG, "Mining worker tasks failed to start after retries");
    vTaskDelete(NULL);
}

static void ota_reboot_task(void * arg)
{
    (void) arg;
    indication_service_set_mode(INDICATION_MODE_BREATH);

    esp_err_t stop_err = POWER_MANAGEMENT_set_mining_enabled(false);
    bool mining_stopped = false;
    if (stop_err == ESP_OK) {
        uint32_t waited_ms = 0;
        while (waited_ms < OTA_MINING_STOP_TIMEOUT_MS) {
            if (!s_gs->ASIC_initalized) {
                mining_stopped = true;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(OTA_MINING_STOP_POLL_MS));
            waited_ms += OTA_MINING_STOP_POLL_MS;
        }
    }

    if (!mining_stopped) {
        ESP_LOGW(TAG, "Mining did not stop in time (%s), forcing ASIC power down", esp_err_to_name(stop_err));
        create_jobs_task_pause();
        s_gs->SYSTEM_MODULE.mining_paused = true;
        SYSTEM_mark_mining_stopped(s_gs);
        VCORE_set_voltage(s_gs, 0.0f);
        asic_hold_reset_low();
        s_gs->ASIC_initalized = false;
    } else {
        ESP_LOGI(TAG, "Mining stopped successfully before OTA reboot");
    }

    ESP_LOGW(TAG, "OTA update applied. Rebooting in %lu ms", (unsigned long) OTA_REBOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}

static void ota_event_handler(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    (void) arg;
    (void) event_data;

    if (event_base != OTA_EVENTS) {
        return;
    }

    if (event_id == OTA_EVENT_READY_FOR_REBOOT) {
        if (s_ota_reboot_scheduled) {
            return;
        }
        s_ota_reboot_scheduled = true;

        if (xTaskCreate(ota_reboot_task, "ota_reboot", 3072, NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA reboot task, restarting immediately");
            esp_restart();
        }
        return;
    }

    if (event_id == OTA_EVENT_CHECK_FW_FAILED) {
        ESP_LOGW(TAG, "OTA image validation failed");
    }
}

// Starts default Wi-Fi connection from persisted state_service credentials.
static void wifi_startup_task(void * arg)
{
    (void) arg;
    esp_err_t wifi_err = wifi_connect_start_default(s_wifi_boot_device_id);
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi not connected on startup: %s", esp_err_to_name(wifi_err));
    }
    vTaskDelete(NULL);
}

// Starts BLE GATT server used for Wi-Fi provisioning.
static void ble_startup_task(void * arg)
{
    (void) arg;
    esp_err_t ble_err = ble_server_start(s_ble_boot_device_id);
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE server failed to start: %s", esp_err_to_name(ble_err));
    }
    vTaskDelete(NULL);
}

// Starts periodic metrics sender service to backend.
static void metrics_startup_task(void * arg)
{
    (void) arg;
    esp_err_t metrics_err = metrics_service_start();
    if (metrics_err != ESP_OK) {
        ESP_LOGW(TAG, "Metrics service failed to start: %s", esp_err_to_name(metrics_err));
    }
    vTaskDelete(NULL);
}

// Starts SNTP-based clock synchronization independent of mining/stratum state.
static void time_sync_startup_task(void * arg)
{
    (void) arg;
    esp_err_t sync_err = time_sync_service_start();
    if (sync_err != ESP_OK) {
        ESP_LOGW(TAG, "Time sync service failed to start: %s", esp_err_to_name(sync_err));
    }
    vTaskDelete(NULL);
}

static void ota_update_task(void * arg)
{

    ESP_LOGI(TAG, "OTA Update create task");
    if (CONFIG_HEATBIT_OTA_SERVER_URL[0] == '\0') {
        ESP_LOGW(TAG, "OTA server is not configured; update checks are off");
        vTaskDelete(NULL);
    }
    const char * dev_model = nvs_config_get_string(NVS_HB_DEVICE_MODEL);
    if (!dev_model) {
        dev_model = "undefined";
    }
    ota_config_t ota_config = {
        .server_url = CONFIG_HEATBIT_OTA_SERVER_URL,
        .version_path = CONFIG_HEATBIT_OTA_VERSION_PATH,
        .download_path = CONFIG_HEATBIT_OTA_DOWNLOAD_PATH,
        .check_interval_s = CONFIG_OTA_CHECK_INTERVAL_S,
        .device_model = dev_model,
        .configure_http = backend_auth_ota_http,
    };

    if (ota_manager_start(&ota_config) != ESP_OK) {
        ESP_LOGE(TAG, "OTA Update task start failed");
    }
    vTaskDelete(NULL);
}

// Starts persistent backend websocket channel for live status updates.
static void ws_backend_startup_task(void * arg)
{
    (void) arg;
    esp_err_t ws_err = ws_backend_service_start();
    if (ws_err != ESP_OK) {
        ESP_LOGW(TAG, "WS backend service failed to start: %s", esp_err_to_name(ws_err));
    }
    vTaskDelete(NULL);
}

// Starts MQTT telemetry/commands service.
static void mqtt_startup_task(void * arg)
{
    (void) arg;
    esp_err_t mqtt_err = mqtt_service_start();
    if (mqtt_err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT service failed to start: %s", esp_err_to_name(mqtt_err));
    }
    vTaskDelete(NULL);
}

void bitair_app_init_early(GlobalState *gs)
{
    s_gs = gs;
    monitor_logging_service_apply_default();

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(loop_err));
        return;
    }
    if (esp_event_handler_register(OTA_EVENTS, ESP_EVENT_ANY_ID, &ota_event_handler, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OTA event handler");
    }

    if (indication_service_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start indication service");
    }

    // Initialize runtime status bridge used by BLE/system status publishing.
    system_status_init();
}

esp_err_t bitair_app_init_state(GlobalState *gs)
{
    s_gs = gs;
    // Initialize persistent device state storage (Wi-Fi/fan/power and related fields).
    if (state_service_init() != ESP_OK || state_listeners_register(s_gs) != ESP_OK || state_service_load() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init/load state_service");
        return ESP_FAIL;
    }
    miner_status_provider_register(s_gs);
    pool_config_applier_register();

    // Device identifiers used by the cloud, BLE and system reporting.
    if (device_id_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init device_id");
        return ESP_FAIL;
    }

    // Identifiers are written into NVS at manufacturing. A device without them
    // still mines, but cannot use the cloud.
    char device_uuid[DEVICE_UUID4_STR_LEN] = {0};
    char worker_id[WORKER_ID_STR_LEN] = {0};
    esp_err_t id_err = device_id_load(device_uuid, worker_id);
    if (id_err != ESP_OK) {
        ESP_LOGW(TAG, "Device is not provisioned (%s): cloud services will not connect", esp_err_to_name(id_err));
    }
    system_status_set_nvs_ids(device_uuid, worker_id);
    strlcpy(s_device_uuid, device_uuid, sizeof(s_device_uuid));

    if (id_err == ESP_OK && device_cert_init(device_uuid) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load device certificate");
    }

    return ESP_OK;
}

void bitair_app_init_board(GlobalState *gs)
{
    s_gs = gs;
    float board_default_freq = (float) s_gs->DEVICE_CONFIG.family.asic.default_frequency_mhz;
    uint16_t board_default_voltage = s_gs->DEVICE_CONFIG.family.asic.default_voltage_mv;
    nvs_config_set_float(NVS_CONFIG_ASIC_FREQUENCY, board_default_freq);
    nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, board_default_voltage);
    ESP_LOGI(TAG, "Applied board defaults: ASIC frequency=%.0f MHz, voltage=%u mV", board_default_freq,
             (unsigned) board_default_voltage);

    POWER_MANAGEMENT_bind_global_state(s_gs);
}

void bitair_app_start_services(GlobalState *gs, esp_err_t system_init_ret)
{
    s_gs = gs;
    if (pwm_duty_monitor_start() != ESP_OK) {
        ESP_LOGE(TAG, "Error starting PWM duty monitor");
    }

    if (system_init_ret == ESP_OK && !s_gs->SELF_TEST_MODULE.is_active) {
        if (xTaskCreate(FAN_CONTROLLER_task, "fan_controller", 8192, (void *) s_gs, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating fan controller task");
        }
    }

    if (!s_gs->SELF_TEST_MODULE.is_active) {
        // Bind legacy GlobalState views to the new Heatbit Wi-Fi/BLE runtime.
        strlcpy(s_ble_boot_device_id, s_device_uuid, sizeof(s_ble_boot_device_id));
        strlcpy(s_wifi_boot_device_id, s_device_uuid, sizeof(s_wifi_boot_device_id));

        if (xTaskCreate(wifi_startup_task, "wifi_startup", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create Wi-Fi startup task");
        }
        if (xTaskCreate(ble_startup_task, "ble_startup", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create BLE startup task");
        }
#if CONFIG_HEATBIT_ENROLL_ENABLED
        if (device_cert_start_enrollment() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start device enrollment");
        }
#endif
        if (tpm300a_service_start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start TPM-300A service");
        }
        if (aht20_service_start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start AHT20 service");
        }
        if (xTaskCreate(time_sync_startup_task, "time_sync_startup", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create time sync startup task");
        }
        if (xTaskCreate(ota_update_task, "ota_update", 4096, (void *) s_gs, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA Update task");
        }
        if (xTaskCreate(metrics_startup_task, "metrics_startup", 4096, (void *) s_gs, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create metrics startup task");
        }
        if (xTaskCreate(ws_backend_startup_task, "ws_backend_startup", 6144, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create WS backend startup task");
        }
        if (xTaskCreate(mqtt_startup_task, "mqtt_startup", 6144, (void *) s_gs, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create MQTT startup task");
        }
    }

    if (system_init_ret == ESP_OK) {
        if (status_reporter_start(s_gs) != ESP_OK) {
            ESP_LOGE(TAG, "Error starting status reporter");
        }
    }
}

void bitair_app_start_mining(GlobalState *gs, esp_err_t *system_init_ret)
{
    s_gs = gs;
    bool jobs_ok = ensure_create_jobs_task_running();
    bool result_ok = ensure_asic_result_task_running();
    if ((!jobs_ok || !result_ok)
        && xTaskCreate(mining_workers_retry_task,
                       "mining_workers_retry",
                       4096,
                       NULL,
                       tskIDLE_PRIORITY + 2,
                       NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mining worker retry task");
    }

    if (xTaskCreateWithCaps(hashrate_monitor_task,
                            "hashrate monitor",
                            8192,
                            (void *) s_gs,
                            5,
                            NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "PSRAM stack create failed for hashrate monitor, retrying with internal RAM");
        if (xTaskCreate(hashrate_monitor_task, "hashrate monitor", 8192, (void *) s_gs, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating hashrate monitor task");
        } else {
            ESP_LOGI(TAG, "hashrate_monitor_task started (internal RAM stack)");
        }
    } else {
        ESP_LOGI(TAG, "hashrate_monitor_task started (PSRAM stack)");
    }


    bool pwm_state_synced = pwm_duty_monitor_wait_for_state_sync(PWM_STATE_SYNC_TIMEOUT_MS) == ESP_OK;
    if (!pwm_state_synced) {
        ESP_LOGW(TAG,
                 "PWM duty monitor did not confirm startup state within %lu ms; mining will stay disabled until live PWM state "
                 "is known",
                 (unsigned long) PWM_STATE_SYNC_TIMEOUT_MS);
    }

    bool startup_power_pin = false;
    bool has_startup_power = state_service_get_power_pin(&startup_power_pin) == ESP_OK;

    bool should_start_mining = pwm_state_synced && has_startup_power && startup_power_pin
        && !CONFIG_FORCE_MINING_DISABLED;

    // The power management task starts and stops mining; here we only set
    // the startup decision and wait for the result of the first start.
    s_gs->SYSTEM_MODULE.mining_paused = !should_start_mining;
    POWER_MANAGEMENT_set_network_ready();

    if (should_start_mining) {
        uint8_t chip_count = POWER_MANAGEMENT_wait_first_start(pdMS_TO_TICKS(FIRST_MINING_START_TIMEOUT_MS));
        if (chip_count == 0) {
            ESP_LOGE(TAG, "Mining did not start: %s",
                     s_gs->SYSTEM_MODULE.asic_status != NULL ? s_gs->SYSTEM_MODULE.asic_status
                                                                    : "no ASIC detected");
            if (s_gs->SELF_TEST_MODULE.is_active) {
                self_test_show_message(s_gs, s_gs->SYSTEM_MODULE.asic_status);
                *system_init_ret = ESP_FAIL;
            }
        }
    }
}

void bitair_app_after_mining_start(GlobalState *gs)
{
    s_gs = gs;
#if CONFIG_BITAIR_VF_CALIBRATION
    extern void vf_calibration_start(GlobalState *gs);
    vf_calibration_start(s_gs);
#endif
}
