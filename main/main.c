#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include <string.h>

#include "adc.h"
#include "asic_reset.h"
#include "bitair_app.h"
#include "device_config.h"
#include "i2c_bitaxe.h"
#include "nvs_config.h"
#include "protocol_coordinator.h"
#include "self_test.h"
#include "system.h"
#include "system_status.h"
#include "task_monitor.h"
#include "power_management_task.h"

static GlobalState GLOBAL_STATE;

static const char * TAG = "bitair";
static const uint32_t PROTOCOL_COORD_TASK_STACK_SIZE = 8192;

void app_main(void)
{
    bitair_app_init_early(&GLOBAL_STATE);

    if (esp_psram_is_initialized()) {
        GLOBAL_STATE.psram_is_available = true;
    } else {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
    }

    if (xTaskCreateWithCaps(cpu_monitor_task, "cpu_monitor", 4096, (void *)&GLOBAL_STATE, 1, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating cpu monitor task");
    }
#ifdef CONFIG_ENABLE_TASK_MONITOR
    if (xTaskCreateWithCaps(task_monitor_task, "task_monitor", 8192, NULL, 1, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating task monitor task");
    }
#endif
  
    // Init I2C
    ESP_ERROR_CHECK(i2c_bitaxe_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    // Initialize RST pin to low early to minimize ASIC power consumption
    ESP_ERROR_CHECK(asic_hold_reset_low());
    ESP_LOGI(TAG, "RST pin initialized to low");

    // wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Init ADC
    ADC_init();

    // initialize the ESP32 NVS
    if (nvs_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    if (bitair_app_init_state(&GLOBAL_STATE) != ESP_OK) {
            return;
    }

    if (device_config_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init device config");
        return;
    }

    bitair_app_init_board(&GLOBAL_STATE);

    if (self_test_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init self test");
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);

    esp_err_t system_init_ret = SYSTEM_init_peripherals(&GLOBAL_STATE);
    GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret = system_init_ret;

    bitair_app_start_services(&GLOBAL_STATE, system_init_ret);
    
    if (system_init_ret == ESP_OK) {
        if (xTaskCreate(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating power management task");
        }
    } else {
        ESP_LOGE(TAG, "Critical peripheral initialization failure (%s). Entering degraded mode.",
                 esp_err_to_name(system_init_ret));
    }

    while (!GLOBAL_STATE.SYSTEM_MODULE.is_connected) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    queue_init(&GLOBAL_STATE.stratum_queue);

    if (system_init_ret == ESP_OK) {
        bitair_app_start_mining(&GLOBAL_STATE, &system_init_ret);
            }

    bitair_app_after_mining_start(&GLOBAL_STATE);

    protocol_coordinator_init(&GLOBAL_STATE);
    if (xTaskCreateWithCaps(protocol_coordinator_task,
                            "protocol coord",
                            PROTOCOL_COORD_TASK_STACK_SIZE,
                            (void *) &GLOBAL_STATE,
                            5,
                            NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Error creating protocol coordinator task");
    }

    if (GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret = system_init_ret;
        if (xTaskCreateWithCaps(self_test_task, "self_test", 8192, (void *) &GLOBAL_STATE, 10, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "Error creating self test task");
        }
    }
}
