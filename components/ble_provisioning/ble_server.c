#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "mbedtls/base64.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_config.h"
#include "ble_server.h"
#include "device_id.h"
#include "indication_service.h"
#include "system_status.h"
#include "wifi_connect.h"

#define HEATBIT_SERVICE_NAME_PREFIX "Heatbit"
#define BINDING_SERVICE_UUID16 0xA100
#define METRICS_SERVICE_UUID16 0xAF00
#define METRICS_CHAR_BASE_UUID16 0xCF00
#define BLE_METRIC_COUNT 8
#define BLE_METRIC_TEXT_LEN 32
#define BLE_CRED_RAW_MAX_LEN 192
#define BLE_FAST_BLINK_ERROR_DURATION_MS 10000U
#define BLE_FAST_BLINK_HALF_PERIOD_MS 100U
#define BLE_FAST_BLINK_CYCLES ((BLE_FAST_BLINK_ERROR_DURATION_MS / (BLE_FAST_BLINK_HALF_PERIOD_MS * 2U)))

static const char *TAG = "ble_server";

void ble_store_config_init(void);

static uint8_t s_addr_type;
static char s_device_id[DEVICE_UUID4_STR_LEN] = {0};
static char s_device_name[20] = HEATBIT_SERVICE_NAME_PREFIX;
static uint8_t s_wifi_status = ST_NOT_CONNECTED;
static char s_wifi_status_text[32] = "not_connected";
static uint8_t s_last_system_command = CMD_DISCONNECT;
static char s_connect_wifi_name[33] = {0};
static char s_connect_wifi_password[65] = {0};
static uint16_t s_wifi_status_val_handle;
static uint16_t s_wifi_status_text_val_handle;
static uint16_t s_metric_val_handles[BLE_METRIC_COUNT] = {0};
static char s_metric_values[BLE_METRIC_COUNT][BLE_METRIC_TEXT_LEN] = {
    "null", "null", "null", "null", "null", "null", "null", "null",
};
static bool s_binding_mode_enabled = true;

static void ble_advertise(void);

static bool ble_looks_like_base64(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len < 4 || (len % 4) != 0) {
        return false;
    }

    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (isalnum((int)c) != 0) {
            continue;
        }

        if (c == '+' || c == '/' || c == '=') {
            continue;
        }

        return false;
    }

    /* All characters are valid base64; length is divisible by 4. */
    return true;
}

static bool ble_decoded_credential_is_text(const char *data, size_t len)
{
    if (data == NULL || len == 0) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)data[i];
        if (c == '\0') {
            return false;
        }

        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return false;
        }
    }

    return true;
}

static bool ble_copy_or_decode_credential(const uint8_t *raw,
                                          uint16_t raw_len,
                                          char *dst,
                                          size_t dst_size,
                                          bool *base64_error,
                                          bool *used_base64)
{
    if (base64_error != NULL) {
        *base64_error = false;
    }
    if (used_base64 != NULL) {
        *used_base64 = false;
    }

    if (raw == NULL || dst == NULL || dst_size == 0 || raw_len == 0) {
        return false;
    }

    if (ble_looks_like_base64(raw, raw_len)) {
        size_t decoded_len = 0;
        int rc = mbedtls_base64_decode((unsigned char *)dst,
                                       dst_size - 1,
                                       &decoded_len,
                                       raw,
                                       raw_len);
        if (rc == 0 && decoded_len > 0 && decoded_len < dst_size &&
            ble_decoded_credential_is_text(dst, decoded_len)) {
            dst[decoded_len] = '\0';
            if (used_base64 != NULL) {
                *used_base64 = true;
            }
            return true;
        }
    }

    if (raw_len >= dst_size) {
        return false;
    }

    memcpy(dst, raw, raw_len);
    dst[raw_len] = '\0';
    return true;
}

static const ble_uuid16_t g_binding_service_uuid = BLE_UUID16_INIT(BINDING_SERVICE_UUID16);
static const ble_uuid16_t g_metrics_service_uuid = BLE_UUID16_INIT(METRICS_SERVICE_UUID16);
static ble_uuid16_t g_metrics_char_uuids[BLE_METRIC_COUNT];
static struct ble_gatt_chr_def g_metrics_chrs[BLE_METRIC_COUNT + 1];

/* UUID: 1a82f409-8ec7-4e77-b339-2a381365cf7d */
static const ble_uuid128_t g_device_id_char_uuid = BLE_UUID128_INIT(
    0x7d, 0xcf, 0x65, 0x13, 0x38, 0x2a, 0x39, 0xb3,
    0x77, 0x4e, 0xc7, 0x8e, 0x09, 0xf4, 0x82, 0x1a);

/* UUID: 507c6e4f-db1f-45da-a649-49afc7d76751 */
static const ble_uuid128_t g_wifi_status_char_uuid = BLE_UUID128_INIT(
    0x51, 0x67, 0xd7, 0xc7, 0xaf, 0x49, 0x49, 0xa6,
    0xda, 0x45, 0x1f, 0xdb, 0x4f, 0x6e, 0x7c, 0x50);

/* UUID: fbd23f33-9fe0-4c95-94b8-2a1f950a8fb7 */
static const ble_uuid128_t g_wifi_status_text_char_uuid = BLE_UUID128_INIT(
    0xb7, 0x8f, 0x0a, 0x95, 0x1f, 0x2a, 0xb8, 0x94,
    0x95, 0x4c, 0xe0, 0x9f, 0x33, 0x3f, 0xd2, 0xfb);

/* UUID: def6381d-69db-4449-b091-74a90cf36da1 */
static const ble_uuid128_t g_system_command_char_uuid = BLE_UUID128_INIT(
    0xa1, 0x6d, 0xf3, 0x0c, 0xa9, 0x74, 0x91, 0xb0,
    0x49, 0x44, 0xdb, 0x69, 0x1d, 0x38, 0xf6, 0xde);

/* UUID: 756c4191-a754-4206-89f4-aa3f5aadc105 */
static const ble_uuid128_t g_connect_wifi_name_char_uuid = BLE_UUID128_INIT(
    0x05, 0xc1, 0xad, 0x5a, 0x3f, 0xaa, 0xf4, 0x89,
    0x06, 0x42, 0x54, 0xa7, 0x91, 0x41, 0x6c, 0x75);

/* UUID: b2b97e9d-0c17-4273-b663-84902aac4089 */
static const ble_uuid128_t g_connect_wifi_password_char_uuid = BLE_UUID128_INIT(
    0x89, 0x40, 0xac, 0x2a, 0x90, 0x84, 0x63, 0xb6,
    0x73, 0x42, 0x17, 0x0c, 0x9d, 0x7e, 0xb9, 0xb2);

/* Manufacturer data (AD type 0xFF): Company ID 0x4EA7 + payload HEATBIT_DEVICE_MODEL */
static const uint8_t g_mfg_data[] = "\xA7\x4E" HEATBIT_DEVICE_MODEL;

static void ble_notify_char(uint16_t val_handle)
{
    if (val_handle == 0) {
        return;
    }

    ble_gatts_chr_updated(val_handle);
}

static void ble_set_device_id(const char *device_id)
{
    if (device_id == NULL) {
        return;
    }

    strlcpy(s_device_id, device_id, sizeof(s_device_id));
}

static void ble_set_wifi_status(uint8_t status)
{
    if (s_wifi_status != status) {
        s_wifi_status = status;
        ble_notify_char(s_wifi_status_val_handle);
    }
}

static void ble_set_wifi_status_text(const char *status_text)
{
    if (status_text == NULL) {
        status_text = "";
    }

    if (strncmp(s_wifi_status_text, status_text, sizeof(s_wifi_status_text)) != 0) {
        strlcpy(s_wifi_status_text, status_text, sizeof(s_wifi_status_text));
        system_status_set_wifi_state(s_wifi_status_text);
        ble_notify_char(s_wifi_status_text_val_handle);
    }
}

static void ble_reset_wifi_binding_state(void)
{
    s_connect_wifi_name[0] = '\0';
    s_connect_wifi_password[0] = '\0';
    system_status_set_wifi_credentials("", "");

    ble_set_wifi_status(ST_NOT_CONNECTED);
    ble_set_wifi_status_text("not_connected");
}

static int ble_device_id_access_cb(uint16_t conn_handle,
                                   uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt,
                                   void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int rc = os_mbuf_append(ctxt->om, s_device_id, strlen(s_device_id));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_metric_access_cb(uint16_t conn_handle,
                                uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt,
                                void *arg)
{
    (void)conn_handle;
    (void)attr_handle;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || arg == NULL) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uintptr_t raw = (uintptr_t)arg;
    if (raw >= BLE_METRIC_COUNT) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t idx = (uint8_t)raw;
    const char *value = s_metric_values[idx];
    int rc = os_mbuf_append(ctxt->om, value, strlen(value));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void ble_metrics_init_defs(void)
{
    memset(g_metrics_char_uuids, 0, sizeof(g_metrics_char_uuids));
    memset(g_metrics_chrs, 0, sizeof(g_metrics_chrs));

    for (uint8_t i = 0; i < BLE_METRIC_COUNT; i++) {
        g_metrics_char_uuids[i].u.type = BLE_UUID_TYPE_16;
        g_metrics_char_uuids[i].value = (uint16_t)(METRICS_CHAR_BASE_UUID16 + i + 1);
        g_metrics_chrs[i].uuid = &g_metrics_char_uuids[i].u;
        g_metrics_chrs[i].access_cb = ble_metric_access_cb;
        g_metrics_chrs[i].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
        g_metrics_chrs[i].arg = (void *)(uintptr_t)i;
        g_metrics_chrs[i].val_handle = &s_metric_val_handles[i];
    }
}

static int ble_wifi_status_access_cb(uint16_t conn_handle,
                                     uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt,
                                     void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int rc = os_mbuf_append(ctxt->om, &s_wifi_status, sizeof(s_wifi_status));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_wifi_status_text_access_cb(uint16_t conn_handle,
                                          uint16_t attr_handle,
                                          struct ble_gatt_access_ctxt *ctxt,
                                          void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    int rc = os_mbuf_append(ctxt->om, s_wifi_status_text, strlen(s_wifi_status_text));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_system_command_access_cb(uint16_t conn_handle,
                                        uint16_t attr_handle,
                                        struct ble_gatt_access_ctxt *ctxt,
                                        void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (OS_MBUF_PKTLEN(ctxt->om) != 1) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t cmd = 0;
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, &cmd, sizeof(cmd), &copied);
    if (rc != 0 || copied != 1) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (cmd != CMD_DISCONNECT && cmd != CMD_CONNECT && cmd != CMD_RESET && cmd != CMD_DONE) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    s_last_system_command = cmd;
    ESP_LOGI(TAG, "Write command characteristic: cmd=0x%02X", cmd);
    ESP_LOGI(TAG, "Received system command: %d", cmd);

    if (cmd == CMD_DONE) {
        // ble_set_wifi_status(ST_FINISHED);
        // ble_set_wifi_status_text("finished");
        return 0;
    }

    if (cmd == CMD_RESET) {
        ESP_LOGI(TAG, "Reset command: clearing stored Wi-Fi credentials and status characteristics");
        ble_reset_wifi_binding_state();
        return 0;
    }

    if (cmd == CMD_CONNECT) {
        if (s_connect_wifi_name[0] == '\0' || s_connect_wifi_password[0] == '\0') {
            ble_set_wifi_status(ST_CONNECTION_ERROR);
            ble_set_wifi_status_text("missing_credentials");
            ESP_LOGW(TAG, "Missing credentials");
            (void)indication_service_set_breath_speed_multiplier(1);
            (void)indication_service_set_mode(INDICATION_MODE_BREATH);
            (void)indication_service_blink_fast_n(BLE_FAST_BLINK_CYCLES);
            ble_reset_wifi_binding_state();
            return 0;
        }

        ble_set_wifi_status(ST_CONNECTING);
        ble_set_wifi_status_text("connecting_wifi");
        (void)indication_service_set_mode(INDICATION_MODE_BREATH);
        (void)indication_service_set_breath_speed_multiplier(2);

        system_status_set_wifi_credentials(s_connect_wifi_name, s_connect_wifi_password);
        esp_err_t wifi_err = wifi_connect_try_credentials(s_connect_wifi_name, s_connect_wifi_password, 15000);
        (void)indication_service_set_breath_speed_multiplier(1);
        if (wifi_err != ESP_OK) {
            ble_set_wifi_status(ST_CONNECTION_ERROR);
            ble_set_wifi_status_text("connection_error");
            ESP_LOGW(TAG, "Connection error");
            (void)indication_service_set_mode(INDICATION_MODE_BREATH);
            (void)indication_service_blink_fast_n(BLE_FAST_BLINK_CYCLES);
            ble_reset_wifi_binding_state();
            return 0;
        }

        ble_set_wifi_status(ST_CONNECTED);
        ble_set_wifi_status_text("checking_internet");
        ESP_LOGI(TAG, "Checking internet reachability");

        bool reachable = false;
        esp_err_t ping_err = wifi_connect_check_internet_reachability(HEATBIT_API_HOST, 6000, &reachable);
        if (ping_err == ESP_OK && reachable) {
            esp_err_t save_err = wifi_connect_save_credentials(s_connect_wifi_name, s_connect_wifi_password);
            if (save_err != ESP_OK) {
                ESP_LOGW(TAG, "Connected but failed to save Wi-Fi credentials: %s", esp_err_to_name(save_err));
            }
            ble_set_wifi_status(ST_CONNECTED_WITH_INTERNET);
            ble_set_wifi_status_text("connected_with_internet");
            ESP_LOGI(TAG, "Connected with internet access");
            return 0;
        }

        ble_set_wifi_status(ST_CONNECTION_ERROR_NO_INTERNET);
        ble_set_wifi_status_text("no_internet");
        ESP_LOGW(TAG, "No internet connection");
        (void)indication_service_set_mode(INDICATION_MODE_BREATH);
        (void)indication_service_blink_fast_n(BLE_FAST_BLINK_CYCLES);
        ble_reset_wifi_binding_state();
    }

    return 0;
}

static int ble_connect_wifi_name_access_cb(uint16_t conn_handle,
                                           uint16_t attr_handle,
                                           struct ble_gatt_access_ctxt *ctxt,
                                           void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, s_connect_wifi_name, strlen(s_connect_wifi_name));
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t payload_len = OS_MBUF_PKTLEN(ctxt->om);
        if (payload_len == 0 || payload_len > BLE_CRED_RAW_MAX_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t raw_cred[BLE_CRED_RAW_MAX_LEN] = {0};
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, raw_cred, payload_len, &copied);
        if (rc != 0 || copied != payload_len) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        bool base64_error = false;
        bool used_base64 = false;
        if (!ble_copy_or_decode_credential(raw_cred,
                                           payload_len,
                                           s_connect_wifi_name,
                                           sizeof(s_connect_wifi_name),
                                           &base64_error,
                                           &used_base64)) {
            if (base64_error) {
                ble_set_wifi_status(ST_BASE64_ERROR);
                ble_set_wifi_status_text("base64_error");
            }
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        ESP_LOGI(TAG,
                 "Write Wi-Fi name characteristic: raw_len=%u decoded_len=%u encoding=%s ssid=%s",
                 (unsigned)payload_len,
                 (unsigned)strlen(s_connect_wifi_name),
                 used_base64 ? "base64" : "plain",
                 s_connect_wifi_name);

        system_status_set_wifi_credentials(s_connect_wifi_name, s_connect_wifi_password);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_connect_wifi_password_access_cb(uint16_t conn_handle,
                                               uint16_t attr_handle,
                                               struct ble_gatt_access_ctxt *ctxt,
                                               void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, s_connect_wifi_password, strlen(s_connect_wifi_password));
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t payload_len = OS_MBUF_PKTLEN(ctxt->om);
        if (payload_len == 0 || payload_len > BLE_CRED_RAW_MAX_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t raw_cred[BLE_CRED_RAW_MAX_LEN] = {0};
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, raw_cred, payload_len, &copied);
        if (rc != 0 || copied != payload_len) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        bool base64_error = false;
        bool used_base64 = false;
        if (!ble_copy_or_decode_credential(raw_cred,
                                           payload_len,
                                           s_connect_wifi_password,
                                           sizeof(s_connect_wifi_password),
                                           &base64_error,
                                           &used_base64)) {
            if (base64_error) {
                ble_set_wifi_status(ST_BASE64_ERROR);
                ble_set_wifi_status_text("base64_error");
            }
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        size_t pw_len = strlen(s_connect_wifi_password);
        char pw_preview[8] = "***";
        if (pw_len >= 2) {
            pw_preview[0] = s_connect_wifi_password[0];
            pw_preview[1] = s_connect_wifi_password[1];
            pw_preview[2] = '\0';
            strlcat(pw_preview, "***", sizeof(pw_preview));
        }
        ESP_LOGI(TAG,
                 "Write Wi-Fi password characteristic: raw_len=%u decoded_len=%u encoding=%s preview=%s",
                 (unsigned)payload_len,
                 (unsigned)pw_len,
                 used_base64 ? "base64" : "plain",
                 pw_preview);

        system_status_set_wifi_credentials(s_connect_wifi_name, s_connect_wifi_password);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def g_chrs[] = {
    {
        .uuid = &g_device_id_char_uuid.u,
        .access_cb = ble_device_id_access_cb,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &g_wifi_status_char_uuid.u,
        .access_cb = ble_wifi_status_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_wifi_status_val_handle,
    },
    {
        .uuid = &g_wifi_status_text_char_uuid.u,
        .access_cb = ble_wifi_status_text_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_wifi_status_text_val_handle,
    },
    {
        .uuid = &g_system_command_char_uuid.u,
        .access_cb = ble_system_command_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = &g_connect_wifi_name_char_uuid.u,
        .access_cb = ble_connect_wifi_name_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = &g_connect_wifi_password_char_uuid.u,
        .access_cb = ble_connect_wifi_password_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
    },
    {0},
};

static const struct ble_gatt_svc_def g_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_binding_service_uuid.u,
        .characteristics = g_chrs,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_metrics_service_uuid.u,
        .characteristics = g_metrics_chrs,
    },
    {0},
};

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ble_advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        break;
    default:
        break;
    }

    return 0;
}

static void ble_advertise(void)
{
    /* Primary advertising packet includes 0xAF00 and optionally 0xA100 when binding is enabled. */
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    ble_uuid16_t adv_service_uuids[2];
    uint8_t adv_service_count = 0;
    adv_service_uuids[adv_service_count].u.type = BLE_UUID_TYPE_16;
    adv_service_uuids[adv_service_count++].value = METRICS_SERVICE_UUID16;
    if (s_binding_mode_enabled) {
        adv_service_uuids[adv_service_count].u.type = BLE_UUID_TYPE_16;
        adv_service_uuids[adv_service_count++].value = BINDING_SERVICE_UUID16;
    }

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = adv_service_uuids;
    fields.num_uuids16 = adv_service_count;
    fields.uuids16_is_complete = 1;
    fields.mfg_data = (uint8_t *)g_mfg_data;
    fields.mfg_data_len = sizeof(g_mfg_data) - 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    /* Scan response carries full local name, e.g. Heatbit-1234abcd. */
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    rsp_fields.name = (const uint8_t *)s_device_name;
    rsp_fields.name_len = strlen(s_device_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset: %d", reason);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }
    ble_advertise();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_server_start(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ble_set_device_id(device_id);
    snprintf(s_device_name, sizeof(s_device_name), "%s-%.8s", HEATBIT_SERVICE_NAME_PREFIX, device_id);
    ble_metrics_init_defs();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();

    rc = ble_gatts_count_cfg(g_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(g_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_host_task);
    system_status_set_ble_running(true);
    ESP_LOGI(TAG, "BLE server started: name=%s", s_device_name);
    return ESP_OK;
}

void ble_server_set_binding_mode(bool enabled)
{
    if (s_binding_mode_enabled == enabled) {
        return;
    }

    s_binding_mode_enabled = enabled;
    ble_advertise();
}

esp_err_t ble_server_set_metric(uint8_t index, const char *value)
{
    if (index >= BLE_METRIC_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (value == NULL) {
        value = "null";
    }

    if (strncmp(s_metric_values[index], value, sizeof(s_metric_values[index])) == 0) {
        return ESP_OK;
    }

    strlcpy(s_metric_values[index], value, sizeof(s_metric_values[index]));
    ble_notify_char(s_metric_val_handles[index]);
    return ESP_OK;
}

void ble_server_update_wifi_status(uint8_t status, const char *status_text)
{
    ble_set_wifi_status(status);
    ble_set_wifi_status_text(status_text);
}
