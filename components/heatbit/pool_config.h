#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOL_CONFIG_HOST_MAX 128     /* including NUL */
#define POOL_CONFIG_WORKER_MAX 128
#define POOL_CONFIG_PASSWORD_MAX 64

/* Pool settings sent by the backend (pool-config endpoint), validated. */
typedef struct {
    bool has_pool;     /* host, port and tls are set */
    char host[POOL_CONFIG_HOST_MAX];
    uint16_t port;
    uint16_t tls;      /* 0 = plain TCP, 1 = TLS with the bundled CA store */
    bool has_worker;
    char worker[POOL_CONFIG_WORKER_MAX];
    bool has_password;
    char password[POOL_CONFIG_PASSWORD_MAX];
    bool has_power_limit;
    uint16_t power_limit_w;
} pool_config_t;

/*
 * Parses and validates a pool-config response ({"body": {"url", "worker",
 * "password", "psu_power_limit"}}). Invalid fields are left unset (and
 * logged); the others are still returned. ESP_FAIL if the JSON or body is
 * malformed.
 */
esp_err_t pool_config_parse(const char *response_json, pool_config_t *out);

/*
 * Applies a parsed config: stores what changed and restarts mining if the
 * stratum settings changed. Implemented by the application.
 */
typedef esp_err_t (*pool_config_applier_t)(const pool_config_t *config);
void pool_config_set_applier(pool_config_applier_t applier);
esp_err_t pool_config_apply(const pool_config_t *config);

#ifdef __cplusplus
}
#endif
