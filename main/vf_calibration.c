/*
 * Voltage/frequency calibration (development only, CONFIG_BITAIR_VF_CALIBRATION).
 *
 * For each frequency, walks the core voltage down from VF_CAL_V_MAX_MV in
 * VF_CAL_V_STEP_MV steps and measures the hashrate (ASIC counters), the ASIC
 * error rate, core current and temperatures over a window. It stops descending
 * at the first point that is not stable and then moves on. Changes go through
 * NVS so the power management task applies them exactly as in production
 * (voltage raised before frequency, lowered after it). Board defaults are
 * written back to NVS at the next boot anyway.
 *
 * Results are printed as "VFCAL" lines.
 */
#include "sdkconfig.h"

#if CONFIG_BITAIR_VF_CALIBRATION

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "global_state.h"
#include "nvs_config.h"

static const char *TAG = "vf_cal";

#define VF_CAL_V_MAX_MV 1250
#define VF_CAL_V_MIN_MV 1000
#define VF_CAL_V_STEP_MV 25
#define VF_CAL_SETTLE_MS 5000
#define VF_CAL_WINDOW_S 15

// Stability: hashrate within 4% of the expected value and ASIC errors below 1%.
#define VF_CAL_MIN_RATIO 0.96f
#define VF_CAL_MAX_ERROR_PCT 3.0f

// Safety limits: stop raising power when any is reached.
#define VF_CAL_MAX_CHIP_C 72.0f
#define VF_CAL_MAX_VR_C 95.0f
#define VF_CAL_MAX_CURRENT_MA 30000.0f
#define VF_CAL_MIN_VIN_MV 4700.0f

static const uint16_t FREQUENCIES[] = {400, 500, 600, 650, 700, 750, 800, 850};

typedef struct {
    float hashrate;
    float error_pct;
    float current_ma;
    float power_w;
    float chip_c;
    float vr_c;
    float vin_mv;
    bool limit_hit;
    const char *limit;
} vf_sample_t;

static void apply(uint16_t freq, uint16_t mv)
{
    nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, mv);
    nvs_config_set_float(NVS_CONFIG_ASIC_FREQUENCY, freq);
}

static const char *over_limit(GlobalState *gs)
{
    PowerManagementModule *pm = &gs->POWER_MANAGEMENT_MODULE;
    if (pm->chip_temp_avg >= VF_CAL_MAX_CHIP_C) return "LIMIT_CHIP_TEMP";
    if (pm->vr_temp >= VF_CAL_MAX_VR_C) return "LIMIT_VR_TEMP";
    if (pm->current >= VF_CAL_MAX_CURRENT_MA) return "LIMIT_CURRENT";
    if (pm->voltage > 0 && pm->voltage < VF_CAL_MIN_VIN_MV) return "LIMIT_VIN";
    return NULL;
}

/* Settles, then averages over the window; aborts early when a safety limit is hit. */
static vf_sample_t measure(GlobalState *gs)
{
    vf_sample_t s = {0};
    for (int i = 0; i < VF_CAL_SETTLE_MS / 500; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if ((s.limit = over_limit(gs)) != NULL) {
            s.limit_hit = true;
            return s;
        }
    }
    int n = 0;
    for (int i = 0; i < VF_CAL_WINDOW_S; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        PowerManagementModule *pm = &gs->POWER_MANAGEMENT_MODULE;
        s.hashrate += gs->SYSTEM_MODULE.current_hashrate;
        s.error_pct += gs->SYSTEM_MODULE.error_percentage;
        s.current_ma += pm->current;
        s.power_w += pm->power;
        s.chip_c = fmaxf(s.chip_c, pm->chip_temp_avg);
        s.vr_c = fmaxf(s.vr_c, pm->vr_temp);
        s.vin_mv = (n == 0 || pm->voltage < s.vin_mv) ? pm->voltage : s.vin_mv;
        n++;
        if ((s.limit = over_limit(gs)) != NULL) {
            s.limit_hit = true;
            break;
        }
    }
    s.hashrate /= n;
    s.error_pct /= n;
    s.current_ma /= n;
    s.power_w /= n;
    return s;
}

static void log_point(uint16_t freq, int mv, float expected, const vf_sample_t *s, const char *result)
{
    float ratio = expected > 0 ? s->hashrate / expected : 0;
    ESP_LOGW(TAG, "VFCAL,%u,%d,%.0f,%.0f,%.3f,%.2f,%.0f,%.2f,%.1f,%.1f,%.0f,%s", freq, mv, expected, s->hashrate, ratio,
             s->error_pct, s->current_ma, s->power_w, s->chip_c, s->vr_c, s->vin_mv, result);
}

static void vf_calibration_task(void *arg)
{
    GlobalState *gs = (GlobalState *)arg;
    uint16_t orig_mv = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    float orig_freq = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);

    while (!gs->ASIC_initalized || gs->SYSTEM_MODULE.current_hashrate <= 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "VFCAL start: %u points max, window %d s", (unsigned)(sizeof(FREQUENCIES) / sizeof(FREQUENCIES[0])),
             VF_CAL_WINDOW_S);
    ESP_LOGW(TAG, "VFCAL,freq_mhz,mv,expected_ghs,ghs,ratio,err_pct,core_ma,power_w,chip_c,vr_c,vin_mv_min,result");

    // Reference point: the factory setting.
    {
        apply(600, 1150);
        vf_sample_t s = measure(gs);
        float expected = 600.0f * gs->DEVICE_CONFIG.family.asic.small_core_count * gs->DEVICE_CONFIG.family.asic_count / 1000.0f;
        log_point(600, 1150, expected, &s, "REFERENCE");
    }

    // For each frequency, raise the voltage from below until the point is stable:
    // the lowest-power points come first, so thermal limits are reached last.
    int start_mv = VF_CAL_V_MIN_MV;
    for (size_t f = 0; f < sizeof(FREQUENCIES) / sizeof(FREQUENCIES[0]); f++) {
        uint16_t freq = FREQUENCIES[f];
        float expected = freq * gs->DEVICE_CONFIG.family.asic.small_core_count * gs->DEVICE_CONFIG.family.asic_count / 1000.0f;
        int stable_mv = 0;
        bool limit = false;

        for (int mv = start_mv; mv <= VF_CAL_V_MAX_MV; mv += VF_CAL_V_STEP_MV) {
            if (!gs->ASIC_initalized) {
                ESP_LOGE(TAG, "VFCAL ASIC stopped (emergency?), aborting");
                limit = true;
                break;
            }
            apply(freq, (uint16_t)mv);
            vf_sample_t s = measure(gs);
            float ratio = expected > 0 ? s.hashrate / expected : 0;
            if (s.limit_hit) {
                log_point(freq, mv, expected, &s, s.limit);
                limit = true;
                break;
            }
            bool stable = ratio >= VF_CAL_MIN_RATIO && s.error_pct < VF_CAL_MAX_ERROR_PCT;
            log_point(freq, mv, expected, &s, stable ? "STABLE" : "UNSTABLE");
            if (stable) {
                stable_mv = mv;
                break;
            }
        }
        ESP_LOGW(TAG, "VFCAL_MIN,%u,%d", freq, stable_mv);
        if (limit || stable_mv == 0) {
            break; // higher frequencies need more power still
        }
        start_mv = stable_mv - 2 * VF_CAL_V_STEP_MV;
        if (start_mv < VF_CAL_V_MIN_MV) {
            start_mv = VF_CAL_V_MIN_MV;
        }
    }

    ESP_LOGW(TAG, "VFCAL done, restoring %.0f MHz / %u mV", orig_freq, orig_mv);
    apply((uint16_t)orig_freq, orig_mv);
    vTaskDelete(NULL);
}

void vf_calibration_start(GlobalState *gs)
{
    xTaskCreate(vf_calibration_task, "vf_cal", 4096, gs, 3, NULL);
}

#endif
