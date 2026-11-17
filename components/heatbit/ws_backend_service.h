#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "state_service.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ws_backend_service_start(void);
esp_err_t ws_backend_service_stop(void);
esp_err_t ws_backend_service_publish_status(const char *work_mode,
											const char *fan_mode,
											bool power_pin,
                                            const char *version,
                                            uint32_t mhs_5s,
                                            float miner_consumption,
                                            float voc,
                                            float gas,
                                            float co2);
/* State listener: publishes power, fan and work mode changes to the backend. */
void ws_backend_service_state_listener(state_change_t change, const state_t *snapshot);
esp_err_t ws_backend_service_get_runtime_status(bool *out_started,
                                                bool *out_connected,
                                                int64_t *out_last_tx_us,
                                                int64_t *out_last_rx_us);

#ifdef __cplusplus
}
#endif
