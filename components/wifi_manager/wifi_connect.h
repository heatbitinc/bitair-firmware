#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif


esp_err_t wifi_connect_start_default(const char *device_id);
esp_err_t wifi_connect_try_credentials(const char *ssid, const char *password, uint32_t timeout_ms);
esp_err_t wifi_connect_restore_default(uint32_t timeout_ms);
esp_err_t wifi_connect_check_internet_reachability(const char *host, uint32_t timeout_ms, bool *reachable);
esp_err_t wifi_connect_save_credentials(const char *ssid, const char *password);

bool wifi_is_connected();
/* True between IP_EVENT_STA_GOT_IP and the next disconnect. */
bool wifi_connect_has_ip(void);
/* Last IPv4 address as text; kept after a disconnect. */
void wifi_connect_get_ip_str(char out[16]);
esp_err_t get_wifi_current_rssi(int8_t *rssi);


#ifdef __cplusplus
}
#endif
