#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

#include "state_service.h"
#include "system_status.h"
#include "wifi_connect.h"

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_RECONNECT_DELAY_MS 10000
#define WIFI_FACTORY_SSID CONFIG_ESP_WIFI_SSID
#define WIFI_FACTORY_PASSWORD CONFIG_ESP_WIFI_PASSWORD
#define WIFI_HOSTNAME_PREFIX "heatbit-"
#define WIFI_SCAN_MAX_AP_COUNT 20

static const char *TAG = "wifi_connect";

static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_initialized;
static bool s_wifi_handlers_registered;
static esp_netif_t *s_sta_netif;
static char s_hostname[32] = {0};
static char s_target_ssid[33] = {0};
static char s_custom_ssid[33] = {0};
static char s_custom_password[65] = {0};
static uint8_t s_custom_failure_count;
static bool s_using_factory_fallback;
static SemaphoreHandle_t s_connect_op_lock;
static SemaphoreHandle_t s_wifi_init_lock;
static esp_event_handler_instance_t s_wifi_event_instance_any_id;
static esp_event_handler_instance_t s_ip_event_instance_got_ip;
static portMUX_TYPE s_connect_req_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_connect_request_id;
static portMUX_TYPE s_connect_switch_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_connect_switch_in_progress;

/*
 * Reconnect after a disconnect runs in its own task: the event handler only
 * decides which network to try next and schedules the attempt. s_creds_lock
 * guards the credentials and fallback state shared by the event handler, the
 * retry task and callers of wifi_apply_config_and_connect().
 */
#define WIFI_RETRY_TASK_STACK 3072
static portMUX_TYPE s_creds_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_retry_task;
static bool s_retry_pending;
static TickType_t s_retry_at;

// Keep Wi-Fi init resilient on memory-constrained boots.
// We observed esp_wifi_init failing when requesting 16 static RX buffers
// (allocated fewer, then internal deinit path crashed inside IDF).
// Using a slightly lower static RX count avoids that failure mode.
static const int WIFI_SAFE_STATIC_RX_BUF_NUM = 12;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT3

/*
 * One context per reachability check, shared with the ping session's callbacks.
 * The caller may time out before the session ends, so the context is
 * reference-counted and freed by whichever side lets go last.
 */
typedef struct {
    SemaphoreHandle_t done;
    volatile uint32_t received;
    int refs;
} ping_ctx_t;

static void ping_ctx_release(ping_ctx_t *ctx)
{
    if (__atomic_sub_fetch(&ctx->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }
}

/* IPv4 address while associated with an address; written by the event handler. */
static bool s_has_ip;
static char s_ip_str[16];

bool wifi_connect_has_ip(void)
{
    taskENTER_CRITICAL(&s_creds_lock);
    bool has_ip = s_has_ip;
    taskEXIT_CRITICAL(&s_creds_lock);
    return has_ip;
}

void wifi_connect_get_ip_str(char out[16])
{
    taskENTER_CRITICAL(&s_creds_lock);
    strlcpy(out, s_ip_str, 16);
    taskEXIT_CRITICAL(&s_creds_lock);
}

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    if (ctx != NULL) {
        ctx->received++;
    }
}

/* The session is deleted only here, after its last callback. */
static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    if (ctx != NULL) {
        xSemaphoreGive(ctx->done);
        ping_ctx_release(ctx);
    }
    esp_ping_delete_session(hdl);
}

static uint32_t wifi_next_connect_request_id(void)
{
    uint32_t request_id;
    taskENTER_CRITICAL(&s_connect_req_lock);
    s_connect_request_id++;
    request_id = s_connect_request_id;
    taskEXIT_CRITICAL(&s_connect_req_lock);
    return request_id;
}

static uint32_t wifi_current_connect_request_id(void)
{
    uint32_t request_id;
    taskENTER_CRITICAL(&s_connect_req_lock);
    request_id = s_connect_request_id;
    taskEXIT_CRITICAL(&s_connect_req_lock);
    return request_id;
}

static esp_err_t wifi_load_kconfig_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    if (ssid == NULL || password == NULL || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ssid[0] = '\0';
    password[0] = '\0';

#ifdef CONFIG_ESP_WIFI_SSID
    if (CONFIG_ESP_WIFI_SSID[0] != '\0') {
        strlcpy(ssid, CONFIG_ESP_WIFI_SSID, ssid_size);
    }
#endif

#ifdef CONFIG_ESP_WIFI_PASSWORD
    strlcpy(password, CONFIG_ESP_WIFI_PASSWORD, password_size);
#endif

    if (ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static esp_err_t wifi_load_saved_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    if (ssid == NULL || password == NULL || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    char state_ssid[STATE_SSID_LEN] = {0};
    char state_password[STATE_PASSWORD_LEN] = {0};
    esp_err_t err = state_service_get_wifi_ssid(state_ssid);
    if (err != ESP_OK || state_ssid[0] == '\0') {
        return wifi_load_kconfig_credentials(ssid, ssid_size, password, password_size);
    }

    (void)state_service_get_wifi_password(state_password);

    strlcpy(ssid, state_ssid, ssid_size);
    strlcpy(password, state_password, password_size);

    if (ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static bool wifi_credentials_are_factory(const char *ssid, const char *password)
{
    return ssid != NULL && password != NULL &&
           strcmp(ssid, WIFI_FACTORY_SSID) == 0 &&
           strcmp(password, WIFI_FACTORY_PASSWORD) == 0;
}

static esp_err_t wifi_set_target_and_connect(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }

    taskENTER_CRITICAL(&s_creds_lock);
    strlcpy(s_target_ssid, ssid, sizeof(s_target_ssid));
    taskEXIT_CRITICAL(&s_creds_lock);
    system_status_set_wifi_credentials(ssid, password);
    return esp_wifi_connect();
}

static void wifi_schedule_retry(uint32_t delay_ms)
{
    taskENTER_CRITICAL(&s_creds_lock);
    s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    s_retry_pending = true;
    taskEXIT_CRITICAL(&s_creds_lock);
    if (s_retry_task != NULL) {
        xTaskNotifyGive(s_retry_task);
    }
}

static void wifi_cancel_retry(void)
{
    taskENTER_CRITICAL(&s_creds_lock);
    s_retry_pending = false;
    taskEXIT_CRITICAL(&s_creds_lock);
}

static void wifi_retry_task(void *arg)
{
    (void)arg;
    while (true) {
        TickType_t wait = portMAX_DELAY;
        bool due = false;
        char ssid[33] = {0};
        char password[65] = {0};

        taskENTER_CRITICAL(&s_creds_lock);
        if (s_retry_pending) {
            int32_t left = (int32_t)(s_retry_at - xTaskGetTickCount());
            if (left <= 0) {
                due = true;
                s_retry_pending = false;
                if (s_using_factory_fallback) {
                    strlcpy(ssid, WIFI_FACTORY_SSID, sizeof(ssid));
                    strlcpy(password, WIFI_FACTORY_PASSWORD, sizeof(password));
                } else {
                    strlcpy(ssid, s_custom_ssid, sizeof(ssid));
                    strlcpy(password, s_custom_password, sizeof(password));
                }
            } else {
                wait = (TickType_t)left;
            }
        }
        taskEXIT_CRITICAL(&s_creds_lock);

        if (!due) {
            ulTaskNotifyTake(pdTRUE, wait);
            continue;
        }
        if (ssid[0] == '\0') {
            continue;
        }

        ESP_LOGI(TAG, "Retrying Wi-Fi connection to SSID: %s", ssid);
        esp_err_t err = wifi_set_target_and_connect(ssid, password);
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "Retry connect failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t wifi_apply_config_and_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!s_wifi_initialized || s_wifi_event_group == NULL || s_connect_op_lock == NULL || ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t request_id = wifi_next_connect_request_id();

    if (xSemaphoreTake(s_connect_op_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    wifi_config.sta.threshold.authmode = (password != NULL && password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    /* A retry scheduled for the previous network must not override this one. */
    wifi_cancel_retry();
    taskENTER_CRITICAL(&s_creds_lock);
    strlcpy(s_custom_ssid, ssid, sizeof(s_custom_ssid));
    strlcpy(s_custom_password, password != NULL ? password : "", sizeof(s_custom_password));
    s_custom_failure_count = 0;
    s_using_factory_fallback = wifi_credentials_are_factory(s_custom_ssid, s_custom_password);
    strlcpy(s_target_ssid, ssid, sizeof(s_target_ssid));
    taskEXIT_CRITICAL(&s_creds_lock);
    system_status_set_wifi_credentials(ssid, password);
    system_status_set_wifi_state("connecting");

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);

    /* Disconnect first and wait for confirmation so set_config cannot fail
     * with "sta is connecting". Mark switch-in-progress only while we really
     * expect a DISCONNECTED event from this explicit disconnect. */
    taskENTER_CRITICAL(&s_connect_switch_lock);
    s_connect_switch_in_progress = true;
    taskEXIT_CRITICAL(&s_connect_switch_lock);

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK) {
        EventBits_t disc_bits = xEventGroupWaitBits(s_wifi_event_group,
                                                    WIFI_DISCONNECTED_BIT,
                                                    pdFALSE,
                                                    pdFALSE,
                                                    pdMS_TO_TICKS(2000));
        if ((disc_bits & WIFI_DISCONNECTED_BIT) == 0) {
            taskENTER_CRITICAL(&s_connect_switch_lock);
            s_connect_switch_in_progress = false;
            taskEXIT_CRITICAL(&s_connect_switch_lock);
            ESP_LOGW(TAG, "Did not receive DISCONNECTED after explicit disconnect; continuing connect flow");
        }
    } else {
        taskENTER_CRITICAL(&s_connect_switch_lock);
        s_connect_switch_in_progress = false;
        taskEXIT_CRITICAL(&s_connect_switch_lock);
    }

    ESP_LOGI(TAG, "Attempting Wi-Fi connection to SSID: %s", ssid);

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        xSemaphoreGive(s_connect_op_lock);
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        xSemaphoreGive(s_connect_op_lock);
        return err;
    }

    xSemaphoreGive(s_connect_op_lock);

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    TickType_t start_ticks = xTaskGetTickCount();

    while (true) {
        if (wifi_current_connect_request_id() != request_id) {
            ESP_LOGI(TAG, "Wi-Fi connect to SSID %s interrupted by a newer request", ssid);
            return ESP_ERR_INVALID_STATE;
        }

        TickType_t elapsed = xTaskGetTickCount() - start_ticks;
        if (elapsed >= timeout_ticks) {
            break;
        }

        TickType_t remaining = timeout_ticks - elapsed;
        TickType_t wait_ticks = pdMS_TO_TICKS(200);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }
        if (wait_ticks > remaining) {
            wait_ticks = remaining;
        }

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               wait_ticks);

        if (bits & WIFI_CONNECTED_BIT) {
            char connected_ssid[33];
            taskENTER_CRITICAL(&s_creds_lock);
            strlcpy(connected_ssid, s_target_ssid[0] != '\0' ? s_target_ssid : ssid, sizeof(connected_ssid));
            taskEXIT_CRITICAL(&s_creds_lock);
            ESP_LOGI(TAG, "Wi-Fi connected to SSID: %s", connected_ssid);
            system_status_set_wifi_state("connected");
            /* Sync connected SSID to state_service */
            state_service_set_connected_wifi_ssid(connected_ssid);
            return ESP_OK;
        }

    }

    ESP_LOGW(TAG, "Wi-Fi connect timeout (%lu ms) for SSID: %s", (unsigned long)timeout_ms, ssid);
    system_status_set_wifi_state("timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wifi_init_once(const char *device_id)
{
    if (s_wifi_init_lock == NULL) {
        s_wifi_init_lock = xSemaphoreCreateMutex();
        if (s_wifi_init_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_wifi_init_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_wifi_initialized) {
        xSemaphoreGive(s_wifi_init_lock);
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        xSemaphoreGive(s_wifi_init_lock);
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        xSemaphoreGive(s_wifi_init_lock);
        return err;
    }

    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_sta_netif == NULL) {
        xSemaphoreGive(s_wifi_init_lock);
        return ESP_FAIL;
    }

    char device_suffix[9] = "00000000";
    if (device_id != NULL && strlen(device_id) >= 8) {
        memcpy(device_suffix, device_id, 8);
        device_suffix[8] = '\0';
    }

    snprintf(s_hostname, sizeof(s_hostname), "%s%s", WIFI_HOSTNAME_PREFIX, device_suffix);
    err = esp_netif_set_hostname(s_sta_netif, s_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set hostname %s: %s", s_hostname, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi hostname set to: %s", s_hostname);
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        xSemaphoreGive(s_wifi_init_lock);
        return ESP_ERR_NO_MEM;
    }

    s_connect_op_lock = xSemaphoreCreateMutex();
    if (s_connect_op_lock == NULL) {
        xSemaphoreGive(s_wifi_init_lock);
        return ESP_ERR_NO_MEM;
    }

    if (s_retry_task == NULL &&
        xTaskCreate(wifi_retry_task, "wifi_retry", WIFI_RETRY_TASK_STACK, NULL, tskIDLE_PRIORITY + 3, &s_retry_task) != pdPASS) {
        s_retry_task = NULL;
        xSemaphoreGive(s_wifi_init_lock);
        return ESP_ERR_NO_MEM;
    }

    wifi_mode_t existing_mode = WIFI_MODE_NULL;
    bool wifi_driver_ready = (esp_wifi_get_mode(&existing_mode) == ESP_OK);
    if (!wifi_driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.static_rx_buf_num = WIFI_SAFE_STATIC_RX_BUF_NUM;
        ESP_LOGW(TAG,
                 "Wi-Fi init override: static_rx_buf_num=%d (sdkconfig default may be higher)",
                 cfg.static_rx_buf_num);
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            xSemaphoreGive(s_wifi_init_lock);
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi driver already initialized, reusing existing instance");
    }

    if (!s_wifi_handlers_registered) {
        err = esp_event_handler_instance_register(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler,
                                                  NULL,
                                                  &s_wifi_event_instance_any_id);
        if (err != ESP_OK) {
            xSemaphoreGive(s_wifi_init_lock);
            ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_event_handler_instance_register(IP_EVENT,
                                                  IP_EVENT_STA_GOT_IP,
                                                  &wifi_event_handler,
                                                  NULL,
                                                  &s_ip_event_instance_got_ip);
        if (err != ESP_OK) {
            xSemaphoreGive(s_wifi_init_lock);
            ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
            return err;
        }

        s_wifi_handlers_registered = true;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        xSemaphoreGive(s_wifi_init_lock);
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        xSemaphoreGive(s_wifi_init_lock);
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // This device runs continuous network workloads (stratum/ws/http).
    // Disable Wi-Fi modem power save to reduce driver warning spam like
    // "wifi:m f null" and avoid transient latency/jitter.
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save disabled (WIFI_PS_NONE)");
    }

    s_wifi_initialized = true;
    xSemaphoreGive(s_wifi_init_lock);
    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        system_status_set_wifi_state("starting");
        (void)state_service_notify_network_changed();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        (void)event_data;
        system_status_set_wifi_state("disconnected");
        taskENTER_CRITICAL(&s_creds_lock);
        s_has_ip = false;
        taskEXIT_CRITICAL(&s_creds_lock);
        (void)state_service_notify_network_changed();

        bool switching = false;
        taskENTER_CRITICAL(&s_connect_switch_lock);
        switching = s_connect_switch_in_progress;
        if (switching) {
            s_connect_switch_in_progress = false;
        }
        taskEXIT_CRITICAL(&s_connect_switch_lock);

        if (switching) {
            /* Caller (wifi_apply_config_and_connect) is waiting for this signal
             * before it calls set_config + connect with the new SSID. */
            xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            return;
        }

        /* Decide the next network here; the retry task connects, so this handler never blocks. */
        bool to_factory = false;
        bool to_custom = false;
        taskENTER_CRITICAL(&s_creds_lock);
        if (!s_using_factory_fallback && !wifi_credentials_are_factory(s_custom_ssid, s_custom_password)) {
            s_custom_failure_count++;
            if (s_custom_failure_count >= 2 && WIFI_FACTORY_SSID[0] != '\0') {
                s_using_factory_fallback = true;
                to_factory = true;
            }
        } else if (s_using_factory_fallback) {
            s_using_factory_fallback = false;
            to_custom = true;
        }
        taskEXIT_CRITICAL(&s_creds_lock);

        if (to_factory) {
            ESP_LOGW(TAG, "Custom Wi-Fi connection failed; trying factory SSID: %s", WIFI_FACTORY_SSID);
            wifi_schedule_retry(0);
        } else {
            if (to_custom) {
                ESP_LOGW(TAG, "Factory Wi-Fi connection failed; returning to custom SSID");
            }
            ESP_LOGI(TAG, "Retrying Wi-Fi connection in %lu seconds", (unsigned long)(WIFI_RECONNECT_DELAY_MS / 1000));
            wifi_schedule_retry(WIFI_RECONNECT_DELAY_MS);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected to Wi-Fi, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        /* Only consecutive failures of the user network lead to the factory fallback. */
        taskENTER_CRITICAL(&s_creds_lock);
        s_custom_failure_count = 0;
        taskEXIT_CRITICAL(&s_creds_lock);
        system_status_set_wifi_state("connected");
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        taskENTER_CRITICAL(&s_creds_lock);
        s_has_ip = true;
        strlcpy(s_ip_str, ip_str, sizeof(s_ip_str));
        taskEXIT_CRITICAL(&s_creds_lock);
        (void)state_service_notify_network_changed();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/*
 * After a failed attempt with new credentials the driver keeps retrying them.
 * Point the retry logic back at the saved network without blocking the
 * caller (BLE runs this from its host task).
 */
static void wifi_return_to_saved_network(void)
{
    char saved_ssid[33] = {0};
    char saved_password[65] = {0};
    if (wifi_load_saved_credentials(saved_ssid, sizeof(saved_ssid), saved_password, sizeof(saved_password)) != ESP_OK) {
        return;
    }

    taskENTER_CRITICAL(&s_creds_lock);
    strlcpy(s_custom_ssid, saved_ssid, sizeof(s_custom_ssid));
    strlcpy(s_custom_password, saved_password, sizeof(s_custom_password));
    s_custom_failure_count = 0;
    s_using_factory_fallback = wifi_credentials_are_factory(s_custom_ssid, s_custom_password);
    taskEXIT_CRITICAL(&s_creds_lock);

    ESP_LOGW(TAG, "New Wi-Fi credentials failed; returning to saved SSID: %s", saved_ssid);
    wifi_schedule_retry(1000);
    /* Stop the attempt in progress; the DISCONNECTED handler reschedules the retry. */
    (void)esp_wifi_disconnect();
}

esp_err_t wifi_connect_start_default(const char *device_id)
{
    ESP_RETURN_ON_ERROR(wifi_init_once(device_id), TAG, "wifi init failed");

    char saved_ssid[33] = {0};
    char saved_password[65] = {0};
    esp_err_t saved_err = wifi_load_saved_credentials(saved_ssid,
                                                      sizeof(saved_ssid),
                                                      saved_password,
                                                      sizeof(saved_password));
    if (saved_err != ESP_OK) {
        ESP_LOGE(TAG, "Wifi not configured");
        system_status_set_wifi_state("not_configured");
        return saved_err;
    }

    ESP_LOGI(TAG, "Trying saved Wi-Fi credentials from state_service, SSID: %s", saved_ssid);
    return wifi_apply_config_and_connect(saved_ssid, saved_password, WIFI_CONNECT_TIMEOUT_MS);
}

esp_err_t wifi_connect_try_credentials(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        timeout_ms = WIFI_CONNECT_TIMEOUT_MS;
    }
    esp_err_t err = wifi_apply_config_and_connect(ssid, password, timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE: a newer request took over; that request owns the network now. */
        wifi_return_to_saved_network();
    }
    return err;
}

esp_err_t wifi_connect_restore_default(uint32_t timeout_ms)
{
    char saved_ssid[33] = {0};
    char saved_password[65] = {0};
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        timeout_ms = WIFI_CONNECT_TIMEOUT_MS;
    }
    ESP_RETURN_ON_ERROR(wifi_load_saved_credentials(saved_ssid, sizeof(saved_ssid), saved_password, sizeof(saved_password)),
                        TAG,
                        "No saved credentials in state_service");
    return wifi_apply_config_and_connect(saved_ssid, saved_password, timeout_ms);
}

esp_err_t wifi_connect_check_internet_reachability(const char *host, uint32_t timeout_ms, bool *reachable)
{
    if (host == NULL || host[0] == '\0' || reachable == NULL || !s_wifi_initialized || s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *reachable = false;

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, NULL, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGW(TAG, "getaddrinfo failed for %s: %d", host, gai);
        return ESP_FAIL;
    }

    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    struct sockaddr_in *addr4 = (struct sockaddr_in *)res->ai_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4->sin_addr);
    target_addr.type = IPADDR_TYPE_V4;
    freeaddrinfo(res);

    ping_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (ctx->done == NULL) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    ctx->refs = 2; /* caller + session */

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 3;
    ping_config.interval_ms = 250;
    ping_config.timeout_ms = 1000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_end = ping_on_end,
        .cb_args = ctx,
    };

    esp_ping_handle_t ping;
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &ping);
    if (err != ESP_OK) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        return err;
    }

    err = esp_ping_start(ping);
    if (err != ESP_OK) {
        esp_ping_delete_session(ping);
        vSemaphoreDelete(ctx->done);
        free(ctx);
        return err;
    }

    /*
     * On timeout the session is left running: it ends by itself after
     * ping_config.count probes and then releases its reference. Stopping or
     * deleting it here would race with ping_on_end deleting it.
     */
    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ping_ctx_release(ctx);
        return ESP_ERR_TIMEOUT;
    }

    *reachable = (ctx->received > 0);
    ping_ctx_release(ctx);
    return ESP_OK;
}

esp_err_t wifi_connect_save_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(state_service_set_wifi_ssid(ssid), TAG, "Failed to save SSID in state_service");
    ESP_RETURN_ON_ERROR(state_service_set_wifi_password(password), TAG, "Failed to save password in state_service");
    ESP_RETURN_ON_ERROR(state_service_save(), TAG, "Failed to persist credentials in state_service");

    ESP_LOGI(TAG, "Saved Wi-Fi credentials to state_service, SSID: %s", ssid);
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    wifi_ap_record_t ap_info;
    return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
}
esp_err_t get_wifi_current_rssi(int8_t *rssi)
{
    wifi_ap_record_t current_ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&current_ap_info);

    if (err == ESP_OK) {
        *rssi = current_ap_info.rssi;
        return ERR_OK;
    }

    return err;
}

