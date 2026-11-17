#include "indication_service.h"

#include <stdbool.h>

#include "driver/ledc.h"
#include "sdkconfig.h" // IWYU pragma: keep
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_INDICATION_LED_ACTIVE_LEVEL
#define CONFIG_INDICATION_LED_ACTIVE_LEVEL 1
#endif

static const char *TAG = "indication";

static const gpio_num_t INDICATION_LED_GPIO = GPIO_NUM_41;
static const ledc_mode_t INDICATION_LEDC_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t INDICATION_LEDC_TIMER = LEDC_TIMER_0;
static const ledc_channel_t INDICATION_LEDC_CHANNEL = LEDC_CHANNEL_0;
static const ledc_timer_bit_t INDICATION_LEDC_RESOLUTION = LEDC_TIMER_10_BIT;
static const uint32_t INDICATION_LEDC_FREQ_HZ = 5000;
static const uint32_t INDICATION_MAX_DUTY = (1U << 10) - 1U;
static const uint32_t INDICATION_OUTPUT_INVERT = (CONFIG_INDICATION_LED_ACTIVE_LEVEL == 0) ? 1U : 0U;

static const uint32_t SERVICE_TICK_MS = 20;
static const uint32_t FAST_BLINK_HALF_PERIOD_MS = 100;
static const uint32_t SLOW_BLINK_HALF_PERIOD_MS = 500;
static const uint32_t BREATH_STEP_MS = 20;
static const uint32_t BREATH_MIN_PERCENT = 5;
static const uint32_t BREATH_MAX_PERCENT = 100;
static const uint32_t BREATH_STEP_PERCENT = 3;

typedef struct {
    bool active;
    indication_mode_t mode;
    uint32_t pulses_remaining;
    uint64_t last_toggle_ms;
    bool led_on;
    indication_mode_t return_mode;
} temporary_blink_t;

static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_service_mutex = NULL;
static indication_mode_t s_base_mode = INDICATION_MODE_SOLID;
static temporary_blink_t s_temp_blink = {0};

static bool s_breath_increasing = true;
static uint32_t s_breath_percent = BREATH_MIN_PERCENT;
static uint64_t s_last_breath_step_ms = 0;
static uint32_t s_breath_speed_multiplier = 1;

static bool s_continuous_blink_led_on = true;
static uint64_t s_last_continuous_blink_toggle_ms = 0;

static esp_err_t apply_mode(indication_mode_t mode, uint64_t now_ms);

static uint32_t percent_to_duty(uint32_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }

    return (INDICATION_MAX_DUTY * percent) / 100U;
}

static esp_err_t set_led_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(INDICATION_LEDC_MODE, INDICATION_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        return err;
    }

    return ledc_update_duty(INDICATION_LEDC_MODE, INDICATION_LEDC_CHANNEL);
}

static esp_err_t set_led_percent(uint32_t percent)
{
    return set_led_duty(percent_to_duty(percent));
}

static uint32_t mode_half_period_ms(indication_mode_t mode)
{
    return (mode == INDICATION_MODE_BLINK_FAST) ? FAST_BLINK_HALF_PERIOD_MS : SLOW_BLINK_HALF_PERIOD_MS;
}

static bool mode_is_blink(indication_mode_t mode)
{
    return mode == INDICATION_MODE_BLINK_FAST || mode == INDICATION_MODE_BLINK_SLOW;
}

static void reset_continuous_blink_state(void)
{
    s_continuous_blink_led_on = true;
    s_last_continuous_blink_toggle_ms = 0;
}

static void reset_breath_state(void)
{
    s_breath_increasing = true;
    s_breath_percent = BREATH_MIN_PERCENT;
    s_last_breath_step_ms = 0;
}

static esp_err_t apply_continuous_blink(indication_mode_t mode, uint64_t now_ms)
{
    uint32_t half_period_ms = mode_half_period_ms(mode);

    if (s_last_continuous_blink_toggle_ms == 0) {
        s_continuous_blink_led_on = true;
        s_last_continuous_blink_toggle_ms = now_ms;
    } else if ((now_ms - s_last_continuous_blink_toggle_ms) >= half_period_ms) {
        s_continuous_blink_led_on = !s_continuous_blink_led_on;
        s_last_continuous_blink_toggle_ms = now_ms;
    }

    return set_led_percent(s_continuous_blink_led_on ? 100U : 0U);
}

static esp_err_t apply_breath(uint64_t now_ms)
{
    uint32_t step_interval_ms = BREATH_STEP_MS;
    if (s_breath_speed_multiplier > 1U) {
        step_interval_ms = BREATH_STEP_MS / s_breath_speed_multiplier;
        if (step_interval_ms == 0U) {
            step_interval_ms = 1U;
        }
    }

    if (s_last_breath_step_ms == 0 || (now_ms - s_last_breath_step_ms) >= step_interval_ms) {
        s_last_breath_step_ms = now_ms;

        if (s_breath_increasing) {
            uint32_t next = s_breath_percent + BREATH_STEP_PERCENT;
            if (next >= BREATH_MAX_PERCENT) {
                s_breath_percent = BREATH_MAX_PERCENT;
                s_breath_increasing = false;
            } else {
                s_breath_percent = next;
            }
        } else {
            if (s_breath_percent <= BREATH_MIN_PERCENT + BREATH_STEP_PERCENT) {
                s_breath_percent = BREATH_MIN_PERCENT;
                s_breath_increasing = true;
            } else {
                s_breath_percent -= BREATH_STEP_PERCENT;
            }
        }
    }

    return set_led_percent(s_breath_percent);
}

static void begin_temporary_blink(indication_mode_t mode, uint32_t blink_count, indication_mode_t return_mode)
{
    s_temp_blink.active = true;
    s_temp_blink.mode = mode;
    s_temp_blink.pulses_remaining = blink_count;
    s_temp_blink.last_toggle_ms = 0;
    s_temp_blink.led_on = true;
    s_temp_blink.return_mode = return_mode;
}

static esp_err_t apply_temporary_blink(uint64_t now_ms)
{
    uint32_t half_period_ms = mode_half_period_ms(s_temp_blink.mode);

    if (s_temp_blink.last_toggle_ms == 0) {
        s_temp_blink.last_toggle_ms = now_ms;
        return set_led_percent(100U);
    }

    if ((now_ms - s_temp_blink.last_toggle_ms) < half_period_ms) {
        return set_led_percent(s_temp_blink.led_on ? 100U : 0U);
    }

    s_temp_blink.led_on = !s_temp_blink.led_on;
    s_temp_blink.last_toggle_ms = now_ms;

    if (!s_temp_blink.led_on && s_temp_blink.pulses_remaining > 0U) {
        s_temp_blink.pulses_remaining--;
        if (s_temp_blink.pulses_remaining == 0U) {
            s_temp_blink.active = false;
            s_base_mode = s_temp_blink.return_mode;
            reset_breath_state();
            reset_continuous_blink_state();
            return apply_mode(s_base_mode, now_ms);
        }
    }

    return set_led_percent(s_temp_blink.led_on ? 100U : 0U);
}

static esp_err_t apply_mode(indication_mode_t mode, uint64_t now_ms)
{
    switch (mode) {
        case INDICATION_MODE_OFF:
            return set_led_percent(0U);
        case INDICATION_MODE_SOLID:
            return set_led_percent(100U);
        case INDICATION_MODE_SOLID_75:
            return set_led_percent(75U);
        case INDICATION_MODE_SOLID_50:
            return set_led_percent(50U);
        case INDICATION_MODE_BREATH:
            return apply_breath(now_ms);
        case INDICATION_MODE_BLINK_FAST:
        case INDICATION_MODE_BLINK_SLOW:
            return apply_continuous_blink(mode, now_ms);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static void indication_service_task(void *arg)
{
    (void)arg;

    while (true) {
        esp_err_t err = ESP_OK;
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

        if (xSemaphoreTake(s_service_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_temp_blink.active) {
                err = apply_temporary_blink(now_ms);
            } else {
                err = apply_mode(s_base_mode, now_ms);
            }
            xSemaphoreGive(s_service_mutex);
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to update indication LED: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(SERVICE_TICK_MS));
    }
}

esp_err_t indication_service_start(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    if (s_service_mutex == NULL) {
        s_service_mutex = xSemaphoreCreateMutex();
        if (s_service_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = INDICATION_LEDC_MODE,
        .duty_resolution = INDICATION_LEDC_RESOLUTION,
        .timer_num = INDICATION_LEDC_TIMER,
        .freq_hz = INDICATION_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "Failed to configure indication LEDC timer");

    ledc_channel_config_t channel_cfg = {
        .gpio_num = INDICATION_LED_GPIO,
        .speed_mode = INDICATION_LEDC_MODE,
        .channel = INDICATION_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = INDICATION_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = INDICATION_OUTPUT_INVERT,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "Failed to configure indication LEDC channel");

    // Default boot indication: BREATH while system is initializing.
    s_base_mode = INDICATION_MODE_BREATH;
    s_temp_blink.active = false;
    s_breath_speed_multiplier = 1;
    reset_breath_state();
    reset_continuous_blink_state();
    ESP_RETURN_ON_ERROR(apply_mode(s_base_mode, (uint64_t)(esp_timer_get_time() / 1000ULL)),
                        TAG,
                        "Failed to set default BREATH indication");

    if (xTaskCreate(indication_service_task, "indication_service", 3072, NULL, 2, &s_task_handle) != pdPASS) {
        s_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "indication_service started on GPIO%d", INDICATION_LED_GPIO);
    return ESP_OK;
}

esp_err_t indication_service_off(void)
{
    return indication_service_set_mode(INDICATION_MODE_OFF);
}

esp_err_t indication_service_set_mode(indication_mode_t mode)
{
    if (mode > INDICATION_MODE_BLINK_SLOW) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_service_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_service_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_base_mode = mode;
    s_temp_blink.active = false;

    if (mode == INDICATION_MODE_BREATH) {
        reset_breath_state();
    }
    if (mode_is_blink(mode)) {
        reset_continuous_blink_state();
    }

    xSemaphoreGive(s_service_mutex);
    return ESP_OK;
}

esp_err_t indication_service_get_mode(indication_mode_t *out_mode)
{
    if (out_mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_service_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_service_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_mode = s_temp_blink.active ? s_temp_blink.mode : s_base_mode;

    xSemaphoreGive(s_service_mutex);
    return ESP_OK;
}

esp_err_t indication_service_set_breath_speed_multiplier(uint32_t multiplier)
{
    if (multiplier == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_service_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_service_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_breath_speed_multiplier = multiplier;
    // Restart phase so speed changes are visible immediately and deterministic.
    reset_breath_state();

    xSemaphoreGive(s_service_mutex);
    return ESP_OK;
}

static esp_err_t indication_service_blink_n(indication_mode_t mode, uint32_t blink_count)
{
    if (!mode_is_blink(mode) || blink_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_service_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_service_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    indication_mode_t return_mode = s_temp_blink.active ? s_temp_blink.return_mode : s_base_mode;
    begin_temporary_blink(mode, blink_count, return_mode);

    xSemaphoreGive(s_service_mutex);
    return ESP_OK;
}

esp_err_t indication_service_blink_fast_n(uint32_t blink_count)
{
    return indication_service_blink_n(INDICATION_MODE_BLINK_FAST, blink_count);
}

esp_err_t indication_service_blink_slow_n(uint32_t blink_count)
{
    return indication_service_blink_n(INDICATION_MODE_BLINK_SLOW, blink_count);
}

const char *indication_service_mode_to_string(indication_mode_t mode)
{
    switch (mode) {
        case INDICATION_MODE_OFF:
            return "off";
        case INDICATION_MODE_SOLID:
            return "solid";
        case INDICATION_MODE_SOLID_75:
            return "solid_75";
        case INDICATION_MODE_SOLID_50:
            return "solid_50";
        case INDICATION_MODE_BREATH:
            return "breath";
        case INDICATION_MODE_BLINK_FAST:
            return "blink_fast";
        case INDICATION_MODE_BLINK_SLOW:
            return "blink_slow";
        default:
            return "unknown";
    }
}
