#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How the device authenticates to the Heatbit backend, per channel: server
 * verification (TLS) and client credentials. This component is the only
 * place that knows the scheme.
 */

typedef enum {
    BACKEND_HTTP_METRICS,
    BACKEND_HTTP_POOL_CONFIG,
    BACKEND_HTTP_OTA,
} backend_http_channel_t;

/* TLS and client authentication for an HTTPS request to the backend API. */
void backend_auth_http(esp_http_client_config_t *config, backend_http_channel_t channel);

/* TLS for the backend WebSocket. */
void backend_auth_ws(esp_websocket_client_config_t *config);

/*
 * Authentication headers for the WebSocket handshake, "Name: value\r\n" lines:
 * "Auth" with a token from the device UUID and worker_id.
 * ESP_ERR_INVALID_STATE if the device has no identifiers.
 */
esp_err_t backend_auth_ws_headers(char *out, size_t out_size);

/*
 * Transport and credentials for the MQTT broker. ESP_ERR_INVALID_STATE if the
 * device has no client certificate.
 */
esp_err_t backend_auth_mqtt(esp_mqtt_client_config_t *config);

/* backend_auth_http() for the OTA server, in the form ota_update expects. */
void backend_auth_ota_http(esp_http_client_config_t *config);

#ifdef __cplusplus
}
#endif
