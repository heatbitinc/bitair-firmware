#include "aht20_service.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"

#include "i2c_bitaxe.h"
#include "state_service.h"

#define AHT20_I2C_ADDRESS 0x38
#define AHT20_MEASUREMENT_TIMEOUT_MS 100
#define AHT20_PROBE_TIMEOUT_MS 100
#define AHT20_TASK_STACK_SIZE 4096
#define AHT20_POLL_PERIOD_TICKS pdMS_TO_TICKS(5000)

#define AHT20_CMD_INIT 0xBE
#define AHT20_CMD_MEASURE 0xAC
#define AHT20_CMD_SOFT_RESET 0xBA

static const char *TAG = "aht20";
static TaskHandle_t s_task_handle = NULL;
static i2c_master_dev_handle_t s_aht20_dev_handle = NULL;
static bool s_started = false;
static bool s_enabled = true;
static bool s_has_values = false;
static float s_temperature_c = 0.0f;
static float s_humidity_pct = 0.0f;
static bool s_device_registered = false;

static esp_err_t aht20_ensure_device(void)
{
    if (s_device_registered && s_aht20_dev_handle != NULL) {
        return ESP_OK;
    }

    esp_err_t err = i2c_bitaxe_add_device(AHT20_I2C_ADDRESS, &s_aht20_dev_handle, "AHT20");
    if (err == ESP_OK) {
        s_device_registered = true;
    }
    return err;
}
static esp_err_t aht20_probe_device(void)
{
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t get_bus_err = i2c_bitaxe_get_master_bus_handle(&bus);
    if (get_bus_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get I2C bus handle for AHT20 probe: %s", esp_err_to_name(get_bus_err));
        return get_bus_err;
    }

    esp_err_t probe_err = i2c_master_probe(bus, AHT20_I2C_ADDRESS, AHT20_PROBE_TIMEOUT_MS);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 probe on 0x%02X failed: %s", AHT20_I2C_ADDRESS, esp_err_to_name(probe_err));
    } else {
        ESP_LOGI(TAG, "AHT20 probe succeeded on 0x%02X", AHT20_I2C_ADDRESS);
    }

    return probe_err;
}

static esp_err_t aht20_write_cmd(i2c_master_dev_handle_t dev, uint8_t cmd, const uint8_t *data, size_t data_len)
{
    uint8_t buffer[5] = {0};
    if (data_len > (sizeof(buffer) - 1U)) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = cmd;
    if (data_len > 0 && data != NULL) {
        memcpy(&buffer[1], data, data_len);
    }

    return i2c_bitaxe_register_write_bytes(dev, buffer, (uint8_t)(1U + data_len));
}

static esp_err_t aht20_reset(i2c_master_dev_handle_t dev)
{
    esp_err_t err = aht20_write_cmd(dev, AHT20_CMD_SOFT_RESET, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t aht20_init_device(i2c_master_dev_handle_t dev)
{
    esp_err_t reset_err = aht20_reset(dev);
    if (reset_err != ESP_OK) {
        return reset_err;
    }

    const uint8_t init_data[2] = {0x08, 0x00};
    esp_err_t err = aht20_write_cmd(dev, AHT20_CMD_INIT, init_data, sizeof(init_data));
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t aht20_read_measurement(i2c_master_dev_handle_t dev, float *out_temperature_c, float *out_humidity_pct)
{
    if (out_temperature_c == NULL || out_humidity_pct == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t trigger[2] = {0x33, 0x00};
    esp_err_t err = aht20_write_cmd(dev, AHT20_CMD_MEASURE, trigger, sizeof(trigger));
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t raw[6] = {0};
    err = i2c_master_receive(dev, raw, sizeof(raw), AHT20_MEASUREMENT_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    if ((raw[0] & 0x80U) != 0U) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t humidity_raw = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | ((uint32_t)raw[3] >> 4);
    uint32_t temp_raw = (((uint32_t)raw[3] & 0x0FU) << 16) | ((uint32_t)raw[4] << 8) | (uint32_t)raw[5];

    *out_humidity_pct = ((float)humidity_raw / 1048576.0f) * 100.0f;
    *out_temperature_c = ((float)temp_raw / 1048576.0f) * 200.0f - 50.0f;

    return ESP_OK;
}

static void aht20_store_sensor_reading(float temp_c, float humidity_pct)
{
    s_temperature_c = temp_c;
    s_humidity_pct = humidity_pct;
    s_has_values = true;

    esp_err_t set_err = state_service_set_climate(temp_c, humidity_pct);
    if (set_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update state sensor_data: %s", esp_err_to_name(set_err));
    }
}

static void aht20_task(void *arg)
{
    (void)arg;

    bool initialized = false;

    while (true) {
        if (!s_enabled) {
            vTaskDelay(AHT20_POLL_PERIOD_TICKS);
            continue;
        }

        if (s_aht20_dev_handle == NULL) {
            ESP_LOGW(
                TAG,
                "AHT20 device handle is not initialized"
            );
            vTaskDelay(AHT20_POLL_PERIOD_TICKS);
            continue;
        }

        if (!initialized) {
            esp_err_t probe_err = aht20_probe_device();
            if (probe_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "AHT20 probe failed before init: %s",
                    esp_err_to_name(probe_err)
                );
                vTaskDelay(AHT20_POLL_PERIOD_TICKS);
                continue;
            }

            esp_err_t init_err =
                aht20_init_device(s_aht20_dev_handle);

            if (init_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "AHT20 init failed: %s",
                    esp_err_to_name(init_err)
                );
                vTaskDelay(AHT20_POLL_PERIOD_TICKS);
                continue;
            }

            initialized = true;
            ESP_LOGI(TAG, "AHT20 initialized");
        }

        float temp_c = 0.0f;
        float humidity_pct = 0.0f;

        esp_err_t read_err = aht20_read_measurement(
            s_aht20_dev_handle,
            &temp_c,
            &humidity_pct
        );

        if (read_err == ESP_OK) {
            aht20_store_sensor_reading(
                temp_c,
                humidity_pct
            );

            ESP_LOGI(
                TAG,
                "AHT20 temp=%.2fC humidity=%.2f%%",
                temp_c,
                humidity_pct
            );
        } else {
            ESP_LOGW(
                TAG,
                "AHT20 measurement failed: %s",
                esp_err_to_name(read_err)
            );

            initialized = false;
        }

        vTaskDelay(AHT20_POLL_PERIOD_TICKS);
    }
}

esp_err_t aht20_service_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

#if !CONFIG_ENABLE_AHT20_SENSOR
    s_enabled = false;
#endif

    if (!s_enabled) {
        ESP_LOGI(TAG, "AHT20 sensor disabled by Kconfig");
        s_started = true;
        return ESP_OK;
    }

    esp_err_t probe_err = aht20_probe_device();
    if (probe_err != ESP_OK) {
        ESP_LOGI(TAG, "AHT20 sensor is not available at 0x%02X; service disabled", AHT20_I2C_ADDRESS);
        s_enabled = false;
        s_started = true;
        return ESP_OK;
    }

    esp_err_t add_err = aht20_ensure_device();
    if (add_err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 device registration failed at startup: %s", esp_err_to_name(add_err));
        s_started = true;
        return add_err;
    }

    BaseType_t ok = xTaskCreate(aht20_task, "aht20_service", AHT20_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 2, &s_task_handle);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}

