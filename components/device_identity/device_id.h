#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_UUID4_STR_LEN 37
#define WORKER_ID_STR_LEN 15

/*
 * Device identifiers, written into NVS at manufacturing (namespace "device"):
 *   uuid       string  device UUID (RFC 4122, lower case)
 *   worker_id  string  worker ID, 14 lower-case hex characters
 * The firmware only reads them.
 */

esp_err_t device_id_init(void);

/*
 * Reads the identifiers and publishes them to the device state.
 * ESP_ERR_NOT_FOUND if the device has no UUID; out_worker_id is empty if the
 * worker ID is missing or malformed.
 */
esp_err_t device_id_load(char out_uuid[DEVICE_UUID4_STR_LEN], char out_worker_id[WORKER_ID_STR_LEN]);

#ifdef __cplusplus
}
#endif
