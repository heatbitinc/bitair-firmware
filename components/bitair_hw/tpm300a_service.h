#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_values;
    uint16_t tvoc_raw;
    uint16_t ch2o_raw;
    uint16_t co2_ppm;
    uint32_t uart_bytes_total;
    uint32_t frames_v1_ok;
    uint32_t frames_v2_ok;
    uint32_t frames_bad_checksum;
    uint32_t frames_bad_format;
    uint32_t ascii_lines_total;
    int active_rx_gpio;
    int active_tx_gpio;
} tpm300a_snapshot_t;

esp_err_t tpm300a_service_start(void);
esp_err_t tpm300a_service_get_snapshot(tpm300a_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif
