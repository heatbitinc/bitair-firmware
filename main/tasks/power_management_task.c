#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "global_state.h"
#include "nvs_config.h"
#include "vcore.h"
#include "thermal.h"
#include "power.h"
#include "asic.h"
#include "system.h"
#include "utils.h"
#include "asic_init.h"
#include "asic_reset.h"
#include "create_jobs_task.h"
#include "driver/uart.h"
#include "TPS546.h"
#include "thermal_governor.h"

#ifndef CONFIG_FORCE_MINING_DISABLED
#define CONFIG_FORCE_MINING_DISABLED 0
#endif

#define POLL_RATE 100
#define MAX_TEMP 90.0
#define THROTTLE_TEMP 75.0
#define SAFE_TEMP 45.0

#define VOLTAGE_START_THROTTLE 4900
#define VOLTAGE_MIN_THROTTLE 3500
#define VOLTAGE_RANGE (VOLTAGE_START_THROTTLE - VOLTAGE_MIN_THROTTLE)

#define TPS546_THROTTLE_TEMP 105.0
#define TPS546_MAX_TEMP 145.0
#define TPS546_RECOVERY_RETRY_MS 10000

// Consecutive failed temperature reads (one per loop, ~100 ms) while the ASIC is
// powered before mining is stopped, and how long the sensors must read again
// before it may restart.
#define TEMP_SENSOR_FAIL_LIMIT 10
#define TEMP_SENSOR_RECOVERY_MS 15000

#define ASIC_REDUCTION 100.0

static const char * TAG = "power_management";
static GlobalState *s_bound_global_state = NULL;

// Frequency throttle while hot; zero (full configured frequency) until the task starts.
static thermal_governor_t s_thermal_governor;

static float configured_frequency_mhz(void)
{
    return nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
}

static float target_frequency_mhz(void)
{
    return thermal_governor_target_mhz(&s_thermal_governor, configured_frequency_mhz());
}

static uint16_t target_voltage_mv(GlobalState * GLOBAL_STATE)
{
    uint16_t configured = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                              ? GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_voltage_mv
                              : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    int32_t drop_mv = (int32_t)(s_thermal_governor.throttle_mhz * CONFIG_THERMAL_GOVERNOR_UV_PER_MHZ / 1000.0f);
    int32_t voltage = (int32_t)configured - drop_mv;
    int32_t floor_mv = CONFIG_THERMAL_GOVERNOR_MIN_VOLTAGE_MV < configured ? CONFIG_THERMAL_GOVERNOR_MIN_VOLTAGE_MV : configured;
    return (uint16_t)(voltage < floor_mv ? floor_mv : voltage);
}

#define FIRST_START_DONE_BIT BIT0
static volatile bool s_network_ready;
static EventGroupHandle_t s_start_events;
static volatile uint8_t s_first_start_chips;
static bool s_first_start_reported;

typedef enum {
    STOP_RAMP_DOWN, // normal stop: wind the frequency down before cutting power
    STOP_IMMEDIATE, // faults and overheat: cut power at once
} stop_mode_t;

static void mining_stop(GlobalState * GLOBAL_STATE, stop_mode_t mode)
{
    ESP_LOGI(TAG, "Stopping mining%s", mode == STOP_IMMEDIATE ? " (immediate)" : "");
    create_jobs_task_pause();
    SYSTEM_mark_mining_stopped(GLOBAL_STATE);
    bool was_initialized = GLOBAL_STATE->ASIC_initalized;

    if (was_initialized && mode == STOP_RAMP_DOWN) {
    // Wind frequency down to 50 MHz before cutting power. This also updates
        // the transition tracker so the ramp starts from 50 MHz on next start.
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = 50;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = 0;

    ASIC_set_frequency(GLOBAL_STATE);
    ASIC_set_nonce_space(GLOBAL_STATE);
    }

    // Cut ASIC power and hold in reset
    VCORE_set_voltage(GLOBAL_STATE, 0.0f);
    asic_hold_reset_low();

    // Mark uninitialized immediately so tasks stop issuing UART commands
    GLOBAL_STATE->ASIC_initalized = false;

    if (was_initialized && mode == STOP_IMMEDIATE) {
        // The chip loses its PLL setting with power: the next start ramps from 50 MHz.
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = 50;
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency = 50;
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = 0;
    }

    if (was_initialized) {
        // Give tasks time to complete any in-progress UART operation. The UART
        // buffers are flushed by mining_start(): flushing here would wait for the
        // result task's pending read (up to 10 s) to time out.
    vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "Mining stopped");
}

static uint8_t mining_start(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE->ASIC_initalized) {
        SYSTEM_mark_mining_started(GLOBAL_STATE);
        create_jobs_task_resume();
        ESP_LOGI(TAG, "ASIC already initialized, skipping recovery start");
        return GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;
    }

    ESP_LOGI(TAG, "Starting mining");

    // Configured voltage, lowered by the thermal governor if it is throttling
    uint16_t voltage = target_voltage_mv(GLOBAL_STATE);
    VCORE_set_voltage(GLOBAL_STATE, (double) voltage / 1000.0);

    // Wait for voltage to stabilize before touching the ASIC
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Clear any accumulated UART garbage before init
    uart_flush(UART_NUM_1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);
    // Stabilization delay of 2000ms prevents race conditions where tasks are
    // just starting to use the ASIC while power management tries to change frequency
    uint8_t chip_count = asic_initialize(GLOBAL_STATE, ASIC_INIT_RECOVERY, 2000);

    if (chip_count > 0) {
        SYSTEM_mark_mining_started(GLOBAL_STATE);
        create_jobs_task_resume();
        ESP_LOGI(TAG, "Mining started successfully (%d chip(s))", chip_count);
    } else {
        ESP_LOGE(TAG, "Mining start failed - ASIC not detected");
    }

    return chip_count;
}

void POWER_MANAGEMENT_bind_global_state(void *global_state)
{
    s_bound_global_state = (GlobalState *)global_state;
}

void POWER_MANAGEMENT_set_network_ready(void)
{
    if (s_start_events == NULL) {
        s_start_events = xEventGroupCreate();
    }
    s_network_ready = true;
}

uint8_t POWER_MANAGEMENT_wait_first_start(TickType_t timeout)
{
    if (s_start_events == NULL) {
        return 0;
    }
    EventBits_t bits = xEventGroupWaitBits(s_start_events, FIRST_START_DONE_BIT, pdFALSE, pdFALSE, timeout);
    return (bits & FIRST_START_DONE_BIT) ? s_first_start_chips : 0;
}

static void report_first_start(uint8_t chip_count)
{
    if (s_first_start_reported || s_start_events == NULL) {
        return;
    }
    s_first_start_reported = true;
    s_first_start_chips = chip_count;
    xEventGroupSetBits(s_start_events, FIRST_START_DONE_BIT);
}

esp_err_t POWER_MANAGEMENT_set_mining_enabled(bool enabled)
{
    if (s_bound_global_state == NULL) {
        ESP_LOGW(TAG, "POWER_MANAGEMENT_set_mining_enabled(%d): global state is not bound", enabled);
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_FORCE_MINING_DISABLED
    (void)enabled;
    s_bound_global_state->SYSTEM_MODULE.mining_paused = true;
    if (s_bound_global_state->ASIC_initalized) {
        mining_stop(s_bound_global_state, STOP_RAMP_DOWN);
    } else {
        SYSTEM_mark_mining_stopped(s_bound_global_state);
    }
    return ESP_OK;
#else
    ESP_LOGI(TAG,
             "POWER_MANAGEMENT_set_mining_enabled(%d): mining_paused(before)=%d",
             enabled,
             s_bound_global_state->SYSTEM_MODULE.mining_paused);

    s_bound_global_state->SYSTEM_MODULE.mining_paused = !enabled;
    return ESP_OK;
#endif
}

static float expected_hashrate(GlobalState * GLOBAL_STATE)
{
    return GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value * GLOBAL_STATE->DEVICE_CONFIG.family.asic.small_core_count * GLOBAL_STATE->DEVICE_CONFIG.family.asic_count / 1000.0;
}

void POWER_MANAGEMENT_init_frequency(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    float frequency = target_frequency_mhz();

    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = frequency;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency = 50.0;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = expected_hashrate(GLOBAL_STATE);
    
    char expected_hashrate_str[16] = {0};
    suffixString(GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate * 1e6, expected_hashrate_str, sizeof(expected_hashrate_str), 0);
    ESP_LOGI(TAG, "ASIC Frequency: %g MHz, Expected hashrate: %sH/s", frequency, expected_hashrate_str);
}

void POWER_MANAGEMENT_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Starting");

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

#if CONFIG_FORCE_MINING_DISABLED
    sys_module->mining_paused = true;
#endif

    thermal_governor_init(&s_thermal_governor, &(thermal_governor_config_t){
        .chip_throttle_c = CONFIG_THERMAL_GOVERNOR_CHIP_THROTTLE_C,
        .chip_recover_c = CONFIG_THERMAL_GOVERNOR_CHIP_RECOVER_C,
        .vr_throttle_c = CONFIG_THERMAL_GOVERNOR_VR_THROTTLE_C,
        .vr_recover_c = CONFIG_THERMAL_GOVERNOR_VR_RECOVER_C,
        .step_mhz = CONFIG_THERMAL_GOVERNOR_STEP_MHZ,
        .down_interval_ms = CONFIG_THERMAL_GOVERNOR_DOWN_INTERVAL_MS,
        .up_interval_ms = CONFIG_THERMAL_GOVERNOR_UP_INTERVAL_MS,
        .min_frequency_mhz = CONFIG_THERMAL_GOVERNOR_MIN_FREQUENCY_MHZ,
    });

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);
    
    float last_asic_frequency = power_management->frequency_value;

    vTaskDelay(500 / portTICK_PERIOD_MS);
    uint16_t last_core_voltage = 0.0;

    bool is_paused = false;
    TickType_t next_tps546_recovery_tick = 0;
    bool tps546_recovery_pending = false;
    uint8_t temp_sensor_failures = 0;
    TickType_t temp_sensor_ok_since = 0;
    bool temp_sensor_ok_seen = false;

    while (1) {

        if (GLOBAL_STATE->SELF_TEST_MODULE.is_finished) {
            ESP_LOGI(TAG, "Stopped");
            vTaskDelete(NULL);
            return;
        }

        power_management->voltage = Power_get_input_voltage(GLOBAL_STATE);
        Power_get_output(GLOBAL_STATE, &power_management->power, &power_management->current);
        power_management->core_voltage = VCORE_get_voltage_mv(GLOBAL_STATE);

        power_management->chip_temp_avg = Thermal_get_chip_temp(GLOBAL_STATE);
        power_management->chip_temp2_avg = Thermal_get_chip_temp2(GLOBAL_STATE);

        power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
        VCORE_check_fault(GLOBAL_STATE);

        // A sensor that stops answering reads as -1 (chip) or keeps its last value
        // (TPS546), which would silently disable the overheat trip. Stop mining
        // after a short run of failed reads and restart only once both read again.
        bool vr_read_ok = !GLOBAL_STATE->DEVICE_CONFIG.TPS546 || TPS546_temperature_read_ok();
        if (!sys_module->temp_sensor_fault) {
            if (GLOBAL_STATE->ASIC_initalized) {
                bool read_failed = power_management->chip_temp_avg == -1 || !vr_read_ok;
                temp_sensor_failures = read_failed ? temp_sensor_failures + 1 : 0;
                if (temp_sensor_failures >= TEMP_SENSOR_FAIL_LIMIT) {
                    ESP_LOGE(TAG, "Temperature sensor not responding (chip=%.1f, vr_ok=%d); stopping mining",
                             power_management->chip_temp_avg, vr_read_ok);
                    sys_module->temp_sensor_fault = true;
                    temp_sensor_ok_seen = false;
                    temp_sensor_failures = 0;
                }
            } else {
                temp_sensor_failures = 0;
            }
        } else {
            TickType_t now = xTaskGetTickCount();
            if (Thermal_chip_sensor_readable(GLOBAL_STATE) && vr_read_ok) {
                if (!temp_sensor_ok_seen) {
                    temp_sensor_ok_seen = true;
                    temp_sensor_ok_since = now;
                } else if ((now - temp_sensor_ok_since) >= pdMS_TO_TICKS(TEMP_SENSOR_RECOVERY_MS)) {
                    ESP_LOGW(TAG, "Temperature sensors respond again; clearing sensor fault");
                    sys_module->temp_sensor_fault = false;
                }
            } else {
                temp_sensor_ok_seen = false;
            }
        }

        // TPS546 fault recovery. power_fault clears by itself once the rail is off,
        // so recovery keeps its own pending flag: mining stays stopped until a
        // recovery attempt succeeds, and attempts are spaced by TPS546_RECOVERY_RETRY_MS.
        if (GLOBAL_STATE->DEVICE_CONFIG.TPS546 &&
            (sys_module->power_fault || tps546_recovery_pending) &&
            !sys_module->mining_paused &&
            !sys_module->hardware_fault &&
            !sys_module->temp_sensor_fault) {
            TickType_t now = xTaskGetTickCount();

            // Stop immediately once fault is latched so ASIC tasks do not run
            // against a dead rail while waiting for the next recovery slot.
            if (!is_paused) {
                ESP_LOGW(TAG, "TPS546 fault latched; stopping mining immediately before recovery");
                mining_stop(GLOBAL_STATE, STOP_IMMEDIATE);
                is_paused = true;
            }
            if (!tps546_recovery_pending) {
                tps546_recovery_pending = true;
                next_tps546_recovery_tick = now;
            }

            if ((int32_t)(now - next_tps546_recovery_tick) >= 0) {
                ESP_LOGW(TAG, "TPS546 fault detected while power is enabled; attempting regulator recovery");

                if (TPS546_clear_faults() != ESP_OK) {
                    ESP_LOGE(TAG, "TPS546_clear_faults failed");
                }

                vTaskDelay(100 / portTICK_PERIOD_MS);

                uint8_t chip_count = mining_start(GLOBAL_STATE);
                if (chip_count > 0) {
                    sys_module->power_fault = 0;
                    tps546_recovery_pending = false;
                    is_paused = false;
                    ESP_LOGI(TAG, "TPS546 recovery succeeded");
                } else {
                    mining_stop(GLOBAL_STATE, STOP_IMMEDIATE);
                    ESP_LOGW(TAG, "TPS546 recovery attempt failed, ASIC not detected; next attempt in %d ms",
                             TPS546_RECOVERY_RETRY_MS);
                }

                next_tps546_recovery_tick = xTaskGetTickCount() + pdMS_TO_TICKS(TPS546_RECOVERY_RETRY_MS);
            }
        }

        // User pause, hardware fault, all pools unreachable, or TPS546 recovery in progress
        // Nothing runs before the network is up: until then VCORE stays off.
        bool wants_stop = sys_module->mining_paused || sys_module->hardware_fault || sys_module->pools_unavailable
                          || tps546_recovery_pending || sys_module->temp_sensor_fault || !s_network_ready;
        if (wants_stop && !is_paused) {
            bool fault = sys_module->hardware_fault || sys_module->temp_sensor_fault || tps546_recovery_pending;
            mining_stop(GLOBAL_STATE, fault ? STOP_IMMEDIATE : STOP_RAMP_DOWN);
            is_paused = true;
        } else if (!wants_stop && is_paused) {
            report_first_start(mining_start(GLOBAL_STATE));
            is_paused = false;
        }

        // If we've paused or have a hardware fault, skip doing anything else
        if (is_paused || sys_module->hardware_fault) {
            vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
            continue;
        }

        bool asic_overheat =
            power_management->chip_temp_avg > THROTTLE_TEMP
            || power_management->chip_temp2_avg > THROTTLE_TEMP;

        if ((power_management->vr_temp > TPS546_THROTTLE_TEMP || asic_overheat) && (power_management->frequency_value > 50 || power_management->voltage > 1000)) {
            if (power_management->chip_temp2_avg > 0) {
                ESP_LOGE(TAG, "OVERHEAT! VR: %fC ASIC1: %fC ASIC2: %fC", power_management->vr_temp, power_management->chip_temp_avg, power_management->chip_temp2_avg);
            } else {
                ESP_LOGE(TAG, "OVERHEAT! VR: %fC ASIC: %fC", power_management->vr_temp, power_management->chip_temp_avg);
            }

            nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, true);
            ESP_LOGW(TAG, "Entering safe mode due to overheat condition. System operation halted.");
            mining_stop(GLOBAL_STATE, STOP_IMMEDIATE);
            
            // Note: ASIC temperature readings are invalid when ASIC is powered down (returns -1)
            // For 600-series boards that use ASIC thermal diode, we rely on VR temp and fixed cooling time
            // For boards with EMC internal temp sensor, readings remain valid
            bool asic_temp_valid = GLOBAL_STATE->DEVICE_CONFIG.emc_internal_temp;
            int cooling_cycles = 0;
            const int MIN_COOLING_CYCLES = 6; // Minimum 30 seconds cooling
            
            while (cooling_cycles < MIN_COOLING_CYCLES || power_management->vr_temp > TPS546_THROTTLE_TEMP - 10) {
                vTaskDelay(5000 / portTICK_PERIOD_MS); // Wait 5 seconds
                cooling_cycles++;
                
                power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);
                
                // Only check ASIC temps if they're valid (not using ASIC thermal diode)
                if (asic_temp_valid) {
                    power_management->chip_temp_avg = Thermal_get_chip_temp(GLOBAL_STATE);
                    power_management->chip_temp2_avg = Thermal_get_chip_temp2(GLOBAL_STATE);
                    ESP_LOGW(TAG, "Safe mode active (cycle %d) - VR: %.1f°C ASIC1: %.1f°C ASIC2: %.1f°C",
                             cooling_cycles, power_management->vr_temp, power_management->chip_temp_avg, power_management->chip_temp2_avg);
                    
                    // Continue if ASIC temps still too high
                    if (power_management->chip_temp_avg >  SAFE_TEMP || power_management->chip_temp2_avg > SAFE_TEMP) {
                        cooling_cycles = 0; // Reset cycle count if still hot
                    }
                } else {
                    // For boards using ASIC thermal diode (600 series), rely on VR temp and time
                    ESP_LOGW(TAG, "Safe mode active (cycle %d/%d) - VR: %.1f°C (ASIC temps unavailable while powered down)",
                             cooling_cycles, MIN_COOLING_CYCLES, power_management->vr_temp);
                }
            }
            ESP_LOGI(TAG, "Temperature normalized after %d cooling cycles. Reinitializing ASIC...", cooling_cycles);
            
            // Restart slower instead of lowering the configured frequency and
            // voltage in NVS for good: the governor raises it again once cool.
            thermal_governor_on_overheat(&s_thermal_governor, ASIC_REDUCTION, configured_frequency_mhz(),
                                         pdTICKS_TO_MS(xTaskGetTickCount()));
            ESP_LOGI(TAG, "Restarting at %.0f MHz (configured %.0f MHz)", target_frequency_mhz(), configured_frequency_mhz());

            uint8_t chip_count = mining_start(GLOBAL_STATE);

            if (chip_count > 0) {
                nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, false);
                last_asic_frequency = power_management->frequency_value;
            }
        }

#if CONFIG_THERMAL_GOVERNOR_ENABLED
        if (!GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
            float chip_temp = power_management->chip_temp_avg > power_management->chip_temp2_avg
                                  ? power_management->chip_temp_avg
                                  : power_management->chip_temp2_avg;
            if (thermal_governor_update(&s_thermal_governor, chip_temp, power_management->vr_temp,
                                        configured_frequency_mhz(), pdTICKS_TO_MS(xTaskGetTickCount()))) {
                ESP_LOGW(TAG, "Thermal governor: chip %.1fC, VR %.1fC -> throttle %.0f MHz (target %.0f MHz)",
                         chip_temp, power_management->vr_temp, s_thermal_governor.throttle_mhz, target_frequency_mhz());
            }
        }
#endif

        // Configured values, adjusted by the thermal governor. When slowing down,
        // lower the frequency before the voltage; when speeding up, raise the
        // voltage before the frequency.
        uint16_t core_voltage = target_voltage_mv(GLOBAL_STATE);
        float asic_frequency = target_frequency_mhz();
        bool frequency_first = asic_frequency < last_asic_frequency;

        for (int pass = 0; pass < 2; pass++) {
            bool frequency_pass = (pass == 0) == frequency_first;

            if (!frequency_pass && core_voltage != last_core_voltage) {
            ESP_LOGI(TAG, "setting new vcore voltage to %umV", core_voltage);
            VCORE_set_voltage(GLOBAL_STATE, (double) core_voltage / 1000.0);
            last_core_voltage = core_voltage;
        }

            if (frequency_pass && asic_frequency != last_asic_frequency) {
            ESP_LOGI(TAG, "New ASIC frequency requested: %g MHz (current: %g MHz)", asic_frequency, last_asic_frequency);
            
            power_management->frequency_value = asic_frequency;
            power_management->expected_hashrate = expected_hashrate(GLOBAL_STATE);

            ASIC_set_frequency(GLOBAL_STATE);
            ASIC_set_nonce_space(GLOBAL_STATE);
            
            last_asic_frequency = asic_frequency;
        }
        }

        // Check for changing of overheat mode
        bool new_overheat_mode = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE);
        
        if (new_overheat_mode != sys_module->overheat_mode) {
            sys_module->overheat_mode = new_overheat_mode;
            ESP_LOGI(TAG, "Overheat mode updated to: %d", sys_module->overheat_mode);
        }

        // looper:
        vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
    }
}
