#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

#include "system.h"
#include "mqtt_service.h"
#include "nvs_config.h"
#include "vcore.h"
#include "thermal.h"
#include "utils.h"
#include "self_test.h"
#include "work_queue.h"
#include "hashrate_monitor_task.h"
#include "tasks/fan_controller_task.h"

static const char * TAG = "system";

//local function prototypes
static esp_err_t ensure_overheat_mode_config();

/*
 * Pool strings in SYSTEM_MODULE are read without a lock by tasks that keep
 * running across a stratum reload (ASIC result submission, MQTT status). A
 * replaced string is therefore not freed at once but retired and freed on the
 * next reload, long after any reader has finished with it.
 */
#define RETIRED_RUNTIME_STRINGS_MAX 8
static char *s_retired_runtime_strings[RETIRED_RUNTIME_STRINGS_MAX];
static size_t s_retired_runtime_string_count;

static void free_retired_runtime_strings(void)
{
    for (size_t i = 0; i < s_retired_runtime_string_count; i++) {
        free(s_retired_runtime_strings[i]);
        s_retired_runtime_strings[i] = NULL;
    }
    s_retired_runtime_string_count = 0;
}

static void replace_runtime_string(char **target, char *replacement)
{
    if (replacement == NULL) {
        return;
    }

    char *old = *target;
    *target = replacement;
    if (old != NULL) {
        if (s_retired_runtime_string_count < RETIRED_RUNTIME_STRINGS_MAX) {
            s_retired_runtime_strings[s_retired_runtime_string_count++] = old;
        } else {
            ESP_LOGW(TAG, "Retired runtime string list full; leaking old value");
        }
    }
}

void SYSTEM_init_system(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if (GLOBAL_STATE->valid_jobs == NULL) {
        GLOBAL_STATE->valid_jobs = calloc(128, sizeof(uint8_t));
        if (GLOBAL_STATE->valid_jobs == NULL) {
            ESP_LOGE(TAG, "Failed to allocate valid_jobs buffer");
        }
    }

    module->shares_submitted = 0;
    module->shares_accepted = 0;
    module->shares_rejected = 0;
    module->last_share_submitted_at_us = 0;
    module->last_share_accepted_at_us = 0;
    module->best_nonce_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF);
    module->best_session_nonce_diff = 0;
    module->start_time = esp_timer_get_time();
    module->lastClockSync = 0;
    module->block_found = 0;
    module->show_new_block = false;

    // Initialize network address strings
    strcpy(module->ip_addr_str, "");
    strcpy(module->ipv6_addr_str, "");
    
    // set the pool url
    module->pool_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL);
    module->fallback_pool_url = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL);

    // set the pool port
    module->pool_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT);
    module->fallback_pool_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT);

    // set the pool tls
    module->pool_tls = nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS);
    module->fallback_pool_tls = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS);

    // set the pool cert
    module->pool_cert = nvs_config_get_string(NVS_CONFIG_STRATUM_CERT);
    module->fallback_pool_cert = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_CERT);

    // set the pool user
    module->pool_user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER);
    module->fallback_pool_user = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER);

    // set the pool password
    module->pool_pass = nvs_config_get_string(NVS_CONFIG_STRATUM_PASS);
    module->fallback_pool_pass = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PASS);

    // set the pool difficulty
    module->pool_difficulty = nvs_config_get_u16(NVS_CONFIG_STRATUM_DIFFICULTY);
    module->fallback_pool_difficulty = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_DIFFICULTY);

    // set the pool extranonce subscribe
    module->pool_extranonce_subscribe = nvs_config_get_bool(NVS_CONFIG_STRATUM_EXTRANONCE_SUBSCRIBE);
    module->fallback_pool_extranonce_subscribe = nvs_config_get_bool(NVS_CONFIG_FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE);

    // set the pool decode coinbase
    module->pool_decode_coinbase_tx = nvs_config_get_bool(NVS_CONFIG_STRATUM_DECODE_COINBASE_TX);
    module->fallback_pool_decode_coinbase_tx = nvs_config_get_bool(NVS_CONFIG_FALLBACK_STRATUM_DECODE_COINBASE_TX);

    // use fallback stratum
    module->use_fallback_stratum = nvs_config_get_bool(NVS_CONFIG_USE_FALLBACK_STRATUM);

    // set based on config
    module->is_using_fallback = module->use_fallback_stratum;

    // load fallback pool protocol
    char *fb_proto_str = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL);
    stratum_protocol_t fb_proto_val = stratum_protocol_from_string(fb_proto_str);
    if (fb_proto_val != STRATUM_PROTOCOL_UNKNOWN) {
        module->fallback_pool_protocol = fb_proto_val;
    } else {
        ESP_LOGW(TAG, "Invalid fallback stratum protocol in NVS: '%s', defaulting to SV1", fb_proto_str ? fb_proto_str : "NULL");
        module->fallback_pool_protocol = STRATUM_PROTOCOL_V1;
    }
    free(fb_proto_str);

    // Initialize pool connection info
    strcpy(module->pool_connection_info, "Not Connected");

    // Initialize overheat_mode
    module->overheat_mode = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE);
    ESP_LOGI(TAG, "Initial overheat_mode value: %d", module->overheat_mode);

    // Start paused: the power management task owns mining start and only
    // starts once app_main has decided at network-ready time (or the gate opens).
    module->mining_paused = true;
    module->mining_started_at_us = 0;
    module->pools_unavailable = false;
    module->stratum_connected = false;

    //Initialize power_fault fault mode
    module->power_fault = 0;

    // set the best diff string
    suffixString(module->best_nonce_diff, module->best_diff_string, DIFF_STRING_SIZE, 0);
    suffixString(module->best_session_nonce_diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);

    // Load stratum protocol selection
    char *proto_str = nvs_config_get_string(NVS_CONFIG_STRATUM_PROTOCOL);
    stratum_protocol_t proto_val = stratum_protocol_from_string(proto_str);
    if (proto_val != STRATUM_PROTOCOL_UNKNOWN) {
        GLOBAL_STATE->stratum_protocol = proto_val;
    } else {
        ESP_LOGW(TAG, "Invalid stratum protocol in NVS: '%s', defaulting to SV1", proto_str ? proto_str : "NULL");
        GLOBAL_STATE->stratum_protocol = STRATUM_PROTOCOL_V1;
    }
    free(proto_str);
    GLOBAL_STATE->sv2_conn = NULL;

    // Initialize mutexes
    pthread_mutex_init(&GLOBAL_STATE->valid_jobs_lock, NULL);
    GLOBAL_STATE->stratum_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
}

void SYSTEM_mark_mining_started(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE == NULL) {
        return;
    }
    
    if (GLOBAL_STATE->SYSTEM_MODULE.mining_started_at_us <= 0) {
        GLOBAL_STATE->SYSTEM_MODULE.mining_started_at_us = esp_timer_get_time();
            }
        }

void SYSTEM_mark_mining_stopped(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE == NULL) {
        return;
    }
    
    GLOBAL_STATE->SYSTEM_MODULE.mining_started_at_us = 0;
}

void SYSTEM_reload_stratum_settings_from_nvs(GlobalState * GLOBAL_STATE)
{
    if (GLOBAL_STATE == NULL) {
        return;
    }

    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;

    free_retired_runtime_strings();
    replace_runtime_string(&module->pool_url, nvs_config_get_string(NVS_CONFIG_STRATUM_URL));
    replace_runtime_string(&module->fallback_pool_url, nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL));

    module->pool_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT);
    module->fallback_pool_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT);

    module->pool_tls = nvs_config_get_u16(NVS_CONFIG_STRATUM_TLS);
    module->fallback_pool_tls = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS);

    replace_runtime_string(&module->pool_user, nvs_config_get_string(NVS_CONFIG_STRATUM_USER));
    replace_runtime_string(&module->fallback_pool_user, nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER));

    replace_runtime_string(&module->pool_pass, nvs_config_get_string(NVS_CONFIG_STRATUM_PASS));
    replace_runtime_string(&module->fallback_pool_pass, nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PASS));

    replace_runtime_string(&module->pool_cert, nvs_config_get_string(NVS_CONFIG_STRATUM_CERT));
    replace_runtime_string(&module->fallback_pool_cert, nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_CERT));

    module->pool_difficulty = nvs_config_get_u16(NVS_CONFIG_STRATUM_DIFFICULTY);
    module->fallback_pool_difficulty = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_DIFFICULTY);

    module->pool_extranonce_subscribe = nvs_config_get_bool(NVS_CONFIG_STRATUM_EXTRANONCE_SUBSCRIBE);
    module->fallback_pool_extranonce_subscribe = nvs_config_get_bool(NVS_CONFIG_FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE);

    module->pool_decode_coinbase_tx = nvs_config_get_bool(NVS_CONFIG_STRATUM_DECODE_COINBASE_TX);
    module->fallback_pool_decode_coinbase_tx = nvs_config_get_bool(NVS_CONFIG_FALLBACK_STRATUM_DECODE_COINBASE_TX);

    module->use_fallback_stratum = nvs_config_get_bool(NVS_CONFIG_USE_FALLBACK_STRATUM);
    module->is_using_fallback = module->use_fallback_stratum;

    char *primary_proto = nvs_config_get_string(NVS_CONFIG_STRATUM_PROTOCOL);
    stratum_protocol_t primary = stratum_protocol_from_string(primary_proto);
    if (primary != STRATUM_PROTOCOL_UNKNOWN) {
        GLOBAL_STATE->stratum_protocol = primary;
    } else {
        ESP_LOGW(TAG, "Ignoring invalid primary stratum protocol in NVS: '%s'", primary_proto != NULL ? primary_proto : "NULL");
    }
    free(primary_proto);

    char *fallback_proto = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL);
    stratum_protocol_t fallback = stratum_protocol_from_string(fallback_proto);
    if (fallback != STRATUM_PROTOCOL_UNKNOWN) {
        module->fallback_pool_protocol = fallback;
    } else {
        ESP_LOGW(TAG, "Ignoring invalid fallback stratum protocol in NVS: '%s'", fallback_proto != NULL ? fallback_proto : "NULL");
    }
    free(fallback_proto);
}

esp_err_t SYSTEM_init_peripherals(GlobalState * GLOBAL_STATE) {
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK) {
        self_test_show_message(GLOBAL_STATE, "ISR:FAIL");
        ESP_LOGE(TAG, "Error installing ISR service");
        return ret;
    }

    ret = ensure_overheat_mode_config();
    if (ret != ESP_OK) {
        self_test_show_message(GLOBAL_STATE, "CONFIG:FAIL");
        ESP_LOGE(TAG, "Failed to ensure overheat_mode config");
        return ret;
    }


    // Initialize the core voltage regulator
    ret = VCORE_init(GLOBAL_STATE);
    if (ret != ESP_OK) {
        self_test_show_message(GLOBAL_STATE, "VCORE:FAIL");
        ESP_LOGE(TAG, "VCORE init failed");
        return ret;
    }

    // For self-test, we set a stable known voltage before ASIC initialization
    if (GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
        vTaskDelay(500 / portTICK_PERIOD_MS);

        ret = VCORE_set_voltage(GLOBAL_STATE, (float)GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_voltage_mv / 1000.0f);
        if (ret != ESP_OK) {
            self_test_show_message(GLOBAL_STATE, "VCORE:FAIL");
            ESP_LOGE(TAG, "VCORE set failed");
            return ret;
        }
    }

    ret = Thermal_init(&GLOBAL_STATE->DEVICE_CONFIG);
    if (ret != ESP_OK) {
        self_test_show_message(GLOBAL_STATE, "THERMAL:FAIL");
        ESP_LOGE(TAG, "Thermal init failed");
        return ret;
    }

    update_fan_speed(GLOBAL_STATE, 0.0f, "power_off");

    return ESP_OK;
}

void SYSTEM_clean_jobs_queue(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Clean Jobs: clearing queue");
    queue_clear(&GLOBAL_STATE->stratum_queue);

    if (GLOBAL_STATE->valid_jobs == NULL) {
        ESP_LOGW(TAG, "valid_jobs is not initialized yet, skip bitmap reset");
        hashrate_monitor_reset_measurements(GLOBAL_STATE);
        return;
    }

    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    for (int i = 0; i < 128; i = i + 4) {
        GLOBAL_STATE->valid_jobs[i] = 0;
    }
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

    // Reset hashrate measurements to prevent a spike on reconnection
    hashrate_monitor_reset_measurements(GLOBAL_STATE);
}

void SYSTEM_notify_accepted_share(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_accepted++;
    module->last_share_accepted_at_us = esp_timer_get_time();
}

void SYSTEM_notify_submitted_share(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_submitted++;
    module->last_share_submitted_at_us = esp_timer_get_time();
}

static int compare_rejected_reason_stats(const void *a, const void *b) {
    const RejectedReasonStat *ea = a;
    const RejectedReasonStat *eb = b;
    return (eb->count > ea->count) - (ea->count > eb->count);
}

void SYSTEM_notify_rejected_share(GlobalState * GLOBAL_STATE, char * error_msg)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_rejected++;
    if (error_msg == NULL || error_msg[0] == '\0') {
        error_msg = "unknown";
    }
    mqtt_service_record_share_reject(error_msg);

    for (int i = 0; i < module->rejected_reason_stats_count; i++) {
        if (strncmp(module->rejected_reason_stats[i].message, error_msg, sizeof(module->rejected_reason_stats[i].message) - 1) == 0) {
            module->rejected_reason_stats[i].count++;
            return;
        }
    }

    if (module->rejected_reason_stats_count < sizeof(module->rejected_reason_stats) / sizeof(module->rejected_reason_stats[0])) {
        strncpy(module->rejected_reason_stats[module->rejected_reason_stats_count].message, 
                error_msg, 
                sizeof(module->rejected_reason_stats[module->rejected_reason_stats_count].message) - 1);
        module->rejected_reason_stats[module->rejected_reason_stats_count].message[sizeof(module->rejected_reason_stats[module->rejected_reason_stats_count].message) - 1] = '\0'; // Ensure null termination
        module->rejected_reason_stats[module->rejected_reason_stats_count].count = 1;
        module->rejected_reason_stats_count++;
    }

    if (module->rejected_reason_stats_count > 1) {
        qsort(module->rejected_reason_stats, module->rejected_reason_stats_count, 
            sizeof(module->rejected_reason_stats[0]), compare_rejected_reason_stats);
    }    
}

void SYSTEM_notify_new_ntime(GlobalState * GLOBAL_STATE, uint32_t ntime)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    // Hourly clock sync
    if (module->lastClockSync + (60 * 60) > ntime) {
        return;
    }
    ESP_LOGI(TAG, "Syncing clock");
    module->lastClockSync = ntime;
    struct timeval tv;
    tv.tv_sec = ntime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

void SYSTEM_notify_found_nonce(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if ((uint64_t) diff > module->best_session_nonce_diff) {
        module->best_session_nonce_diff = (uint64_t) diff;
        suffixString((uint64_t) diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);
    }

    double network_diff = networkDifficulty(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->target);
    if (diff >= network_diff) {
        module->block_found++;
        module->show_new_block = true;
        ESP_LOGI(TAG, "FOUND BLOCK!!!!!!!!!!!!!!!!!!!!!! %f >= %f (count: %d)", diff, network_diff, module->block_found);
    }

    if ((uint64_t) diff <= module->best_nonce_diff) {
        return;
    }
    module->best_nonce_diff = (uint64_t) diff;

    nvs_config_set_u64(NVS_CONFIG_BEST_DIFF, module->best_nonce_diff);

    // make the best_nonce_diff into a string
    suffixString((uint64_t) diff, module->best_diff_string, DIFF_STRING_SIZE, 0);

    ESP_LOGI(TAG, "New best difficulty: %s", module->best_diff_string);
}

static esp_err_t ensure_overheat_mode_config() {
    bool overheat_mode = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE);

    ESP_LOGI(TAG, "Existing overheat_mode value: %d", overheat_mode);

    return ESP_OK;
}

stratum_protocol_t stratum_protocol_from_string(const char *s)
{
    if (!s) return STRATUM_PROTOCOL_UNKNOWN;
    if (strcmp(s, STRATUM_V1) == 0) return STRATUM_PROTOCOL_V1;
    if (strcmp(s, STRATUM_V2) == 0) return STRATUM_PROTOCOL_V2;
    return STRATUM_PROTOCOL_UNKNOWN;
}

sv2_channel_type_t sv2_channel_type_from_string(const char *s)
{
    if (!s) return SV2_CHANNEL_UNKNOWN;
    if (strcmp(s, SV2_CHANNEL_TYPE_EXTENDED) == 0) return SV2_CHANNEL_EXTENDED;
    if (strcmp(s, SV2_CHANNEL_TYPE_STANDARD) == 0) return SV2_CHANNEL_STANDARD;
    return SV2_CHANNEL_UNKNOWN;
}
