#include <stdbool.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "device_id.h"
#include "state_service.h"

static const char *NVS_NS = "device";
static const char *NVS_KEY_UUID = "uuid";
static const char *NVS_KEY_WORKER = "worker_id";

static bool is_hex_string(const char *s, size_t len, bool allow_dashes)
{
    if (strlen(s) != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (allow_dashes && c == '-');
        if (!ok) {
            return false;
        }
    }
    return true;
}

esp_err_t device_id_init(void)
{
    /* NVS holds the device identity: never erase it here. */
    return nvs_flash_init();
}

esp_err_t device_id_load(char out_uuid[DEVICE_UUID4_STR_LEN], char out_worker_id[WORKER_ID_STR_LEN])
{
    if (out_uuid == NULL || out_worker_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out_uuid[0] = '\0';
    out_worker_id[0] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = DEVICE_UUID4_STR_LEN;
    err = nvs_get_str(nvs, NVS_KEY_UUID, out_uuid, &len);
    if (err != ESP_OK || !is_hex_string(out_uuid, DEVICE_UUID4_STR_LEN - 1, true)) {
        out_uuid[0] = '\0';
        nvs_close(nvs);
        return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }

    len = WORKER_ID_STR_LEN;
    if (nvs_get_str(nvs, NVS_KEY_WORKER, out_worker_id, &len) != ESP_OK
        || !is_hex_string(out_worker_id, WORKER_ID_STR_LEN - 1, false)) {
        out_worker_id[0] = '\0';
    }
    nvs_close(nvs);

    state_service_set_device_id(out_uuid);
    state_service_set_worker_id(out_worker_id);
    return ESP_OK;
}
