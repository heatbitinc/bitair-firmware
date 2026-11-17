/*
 * Backend authentication.
 *
 * The server is verified against the ESP-IDF CA bundle. The device presents
 * its client certificate (device_identity/device_cert), issued at
 * manufacturing for <device_uuid>.<CONFIG_HEATBIT_DEVICES_DNS_SUFFIX> and
 * renewed by the device before it expires, on every channel (mutual TLS).
 * Without a certificate the requests are sent without client authentication
 * and the backend rejects them.
 *
 * The WebSocket handshake also carries an "Auth" header: a Fernet token whose
 * plaintext is the worker_id, keyed with the device UUID.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include "backend_auth.h"
#include "device_cert.h"
#include "state_service.h"

static const char *TAG = "backend_auth";

/* device_id and worker_id from the device state. */
static esp_err_t get_ids(char device_id[STATE_UUID_LEN], char worker_id[STATE_WORKER_ID_LEN])
{
    state_t snapshot;
    if (state_service_get_snapshot(&snapshot) != ESP_OK || snapshot.device_id[0] == '\0'
        || snapshot.worker_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(device_id, snapshot.device_id, STATE_UUID_LEN);
    strlcpy(worker_id, snapshot.worker_id, STATE_WORKER_ID_LEN);
    return ESP_OK;
}

static bool extract_uuid_hex(const char *uuid, char out_hex[33])
{
    if (uuid == NULL || out_hex == NULL) {
        return false;
    }

    size_t out_idx = 0;
    for (size_t i = 0; uuid[i] != '\0'; i++) {
        char c = uuid[i];
        if (c == '-') {
            continue;
        }

        if (!isxdigit((unsigned char)c) || out_idx >= 32) {
            return false;
        }

        out_hex[out_idx++] = (char)tolower((unsigned char)c);
    }

    if (out_idx != 32) {
        return false;
    }

    out_hex[32] = '\0';
    return true;
}

static void put_u64_be(uint8_t out[8], uint64_t value)
{
    for (int i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(value & 0xffU);
        value >>= 8;
    }
}

static size_t pkcs7_pad(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size)
{
    size_t pad = 16 - (in_len % 16);
    if (pad == 0) {
        pad = 16;
    }

    size_t total = in_len + pad;
    if (out == NULL || out_size < total) {
        return 0;
    }

    memcpy(out, in, in_len);
    for (size_t i = in_len; i < total; i++) {
        out[i] = (uint8_t)pad;
    }

    return total;
}

static void to_base64_urlsafe(char *s)
{
    if (s == NULL) {
        return;
    }

    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == '+') {
            s[i] = '-';
        } else if (s[i] == '/') {
            s[i] = '_';
        }
    }
}

static esp_err_t encrypt_auth_token(const char *miner_id,
                                    const char *worker_id,
                                    char *out_token,
                                    size_t out_token_size)
{
    if (miner_id == NULL || worker_id == NULL || out_token == NULL || out_token_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char uuid_hex[33] = {0};
    if (!extract_uuid_hex(miner_id, uuid_hex)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t fernet_key[32] = {0};
    memcpy(fernet_key, uuid_hex, sizeof(fernet_key));

    const uint8_t *signing_key = fernet_key;
    const uint8_t *encryption_key = fernet_key + 16;

    const uint8_t *plain = (const uint8_t *)worker_id;
    size_t plain_len = strlen(worker_id);
    uint8_t padded[64] = {0};
    size_t padded_len = pkcs7_pad(plain, plain_len, padded, sizeof(padded));
    if (padded_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t iv[16] = {0};
    esp_fill_random(iv, sizeof(iv));

    uint8_t ciphertext[64] = {0};
    memcpy(ciphertext, padded, padded_len);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int aes_ret = mbedtls_aes_setkey_enc(&aes, encryption_key, 128);
    if (aes_ret != 0) {
        mbedtls_aes_free(&aes);
        return ESP_FAIL;
    }

    uint8_t iv_copy[16] = {0};
    memcpy(iv_copy, iv, sizeof(iv_copy));
    aes_ret = mbedtls_aes_crypt_cbc(&aes,
                                    MBEDTLS_AES_ENCRYPT,
                                    padded_len,
                                    iv_copy,
                                    ciphertext,
                                    ciphertext);
    mbedtls_aes_free(&aes);
    if (aes_ret != 0) {
        return ESP_FAIL;
    }

    uint8_t token_no_hmac[1 + 8 + 16 + 64] = {0};
    size_t offset = 0;
    token_no_hmac[offset++] = 0x80;

    uint64_t now_sec = (uint64_t)(esp_timer_get_time() / 1000000LL);
    put_u64_be(token_no_hmac + offset, now_sec);
    offset += 8;

    memcpy(token_no_hmac + offset, iv, sizeof(iv));
    offset += sizeof(iv);

    memcpy(token_no_hmac + offset, ciphertext, padded_len);
    offset += padded_len;

    uint8_t hmac[32] = {0};
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return ESP_FAIL;
    }

    int hmac_ret = mbedtls_md_hmac(md_info,
                                   signing_key,
                                   16,
                                   token_no_hmac,
                                   offset,
                                   hmac);
    if (hmac_ret != 0) {
        return ESP_FAIL;
    }

    uint8_t full_token[1 + 8 + 16 + 64 + 32] = {0};
    memcpy(full_token, token_no_hmac, offset);
    memcpy(full_token + offset, hmac, sizeof(hmac));
    size_t full_len = offset + sizeof(hmac);

    size_t encoded_len = 0;
    int b64_ret = mbedtls_base64_encode((unsigned char *)out_token,
                                        out_token_size,
                                        &encoded_len,
                                        full_token,
                                        full_len);
    if (b64_ret != 0 || encoded_len >= out_token_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_token[encoded_len] = '\0';
    to_base64_urlsafe(out_token);
    return ESP_OK;
}

void backend_auth_http(esp_http_client_config_t *config, backend_http_channel_t channel)
{
    (void)channel;
    if (config == NULL) {
        return;
    }
    config->crt_bundle_attach = esp_crt_bundle_attach;
    device_cert_apply_client_cert(config);
}

void backend_auth_ws(esp_websocket_client_config_t *config)
{
    if (config == NULL) {
        return;
    }
    config->crt_bundle_attach = esp_crt_bundle_attach;
    const char *cert = device_cert_get_cert_pem();
    const char *key = device_cert_get_key_pem();
    if (cert != NULL && key != NULL) {
        config->client_cert = cert;
        config->client_key = key;
    }
}

esp_err_t backend_auth_ws_headers(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char device_id[STATE_UUID_LEN];
    char worker_id[STATE_WORKER_ID_LEN];
    esp_err_t err = get_ids(device_id, worker_id);
    if (err != ESP_OK) {
        return err;
    }

    char auth_token[192] = {0};
    err = encrypt_auth_token(device_id, worker_id, auth_token, sizeof(auth_token));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Auth token generation failed: %s", esp_err_to_name(err));
        return err;
    }

    int written = snprintf(out, out_size, "Auth: %s\r\n", auth_token);
    if (written <= 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t backend_auth_mqtt(esp_mqtt_client_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *cert = device_cert_get_cert_pem();
    const char *key = device_cert_get_key_pem();
    if (cert == NULL || key == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    config->broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    config->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    config->credentials.authentication.certificate = cert;
    config->credentials.authentication.key = key;
    return ESP_OK;
}

void backend_auth_ota_http(esp_http_client_config_t *config)
{
    backend_auth_http(config, BACKEND_HTTP_OTA);
}
