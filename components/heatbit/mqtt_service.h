#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_get_last_send_time_us(int64_t *out_last_send_us);

void mqtt_service_record_share_reject(const char *reason);

#ifdef __cplusplus
}
#endif
