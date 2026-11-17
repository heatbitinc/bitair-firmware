#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device client certificate (mTLS identity).
 *
 * The certificate names <device_uuid>.<CONFIG_HEATBIT_DEVICES_DNS_SUFFIX> in
 * CN and SAN. The first certificate is written into NVS at manufacturing.
 * Before it expires the device renews it: generates a new EC P-256 key on the
 * chip and sends a CSR to the enrollment service, authenticated with the
 * current certificate.
 *
 * NVS namespace "dev_cert" holds two slots, so a renewal interrupted by a reset
 * never leaves the device without a working pair:
 *   uuid  string  device UUID the pair was issued for
 *   slot  u8      active slot; missing means 0
 *   key   string  slot 0 private key, PEM
 *   crt   string  slot 0 certificate chain (leaf, then issuing CA), PEM
 *   key1  string  slot 1 private key, PEM
 *   crt1  string  slot 1 certificate chain, PEM
 */

/* Loads the certificate from NVS. Call after device_id_load(). */
esp_err_t device_cert_init(const char *device_uuid);

/*
 * Starts the background task that renews the certificate before it expires.
 * ESP_ERR_INVALID_STATE if the device has no certificate to renew.
 */
esp_err_t device_cert_start_enrollment(void);

bool device_cert_is_enrolled(void);

/* Waits until the device has a certificate; returns false on timeout. */
bool device_cert_wait_enrolled(TickType_t timeout);

/*
 * PEM of the device certificate chain and private key, or NULL if the device
 * is not enrolled. Buffers stay valid until reboot, also after a renewal.
 */
const char *device_cert_get_cert_pem(void);
const char *device_cert_get_key_pem(void);

/* Certificate validity end (UTC), or 0 if the device is not enrolled. */
time_t device_cert_get_not_after(void);

/* Presents the device certificate as TLS client certificate, if there is one. */
void device_cert_apply_client_cert(esp_http_client_config_t *config);

#ifdef __cplusplus
}
#endif
