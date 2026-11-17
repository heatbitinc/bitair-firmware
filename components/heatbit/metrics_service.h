#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t metrics_service_start(void);
esp_err_t metrics_service_get_last_send_time_us(int64_t *out_last_send_us);

#ifdef __cplusplus
}
#endif
