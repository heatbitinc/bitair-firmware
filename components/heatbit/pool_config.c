#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "pool_config.h"

static const char *TAG = "pool_config";
static pool_config_applier_t s_applier;

static const char *normalize_pool_url(const char *url, char *buffer, size_t buffer_size)
{
    if (url == NULL || buffer == NULL || buffer_size == 0) {
        return NULL;
    }

    while (*url != '\0' && (unsigned char)*url <= ' ') {
        url++;
    }

    size_t len = strlen(url);
    while (len > 0 && (unsigned char)url[len - 1] <= ' ') {
        len--;
    }

    if (len == 0 || len >= buffer_size) {
        return NULL;
    }

    memcpy(buffer, url, len);
    buffer[len] = '\0';
    return buffer;
}

static bool parse_pool_url_to_host_port(const char *url,
                                        char *out_host,
                                        size_t out_host_size,
                                        uint16_t *out_port)
{
    if (url == NULL || out_host == NULL || out_host_size == 0 || out_port == NULL) {
        return false;
    }

    const char *host_begin = url;
    const char *scheme_sep = strstr(url, "://");
    if (scheme_sep != NULL) {
        host_begin = scheme_sep + 3;
    }

    const char *url_end = host_begin + strlen(host_begin);
    const char *path_sep = strpbrk(host_begin, "/?#");
    if (path_sep != NULL) {
        url_end = path_sep;
    }

    const char *port_sep = NULL;
    for (const char *p = url_end - 1; p >= host_begin; --p) {
        if (*p == ':') {
            port_sep = p;
            break;
        }
        if (p == host_begin) {
            break;
        }
    }

    if (port_sep == NULL || port_sep == host_begin || (port_sep + 1) >= url_end) {
        return false;
    }

    size_t host_len = (size_t)(port_sep - host_begin);
    if (host_len == 0 || host_len >= out_host_size) {
        return false;
    }

    memcpy(out_host, host_begin, host_len);
    out_host[host_len] = '\0';

    char *endptr = NULL;
    long parsed_port = strtol(port_sep + 1, &endptr, 10);
    if (endptr == (port_sep + 1) || endptr != url_end || parsed_port < 1 || parsed_port > UINT16_MAX) {
        return false;
    }

    *out_port = (uint16_t)parsed_port;
    return true;
}

static bool parse_pool_url_to_tls(const char *url,
                                           uint16_t *out_tls)
{
    if (url == NULL || out_tls == NULL) {
        return false;
    }

    static const char *scheme_tcp = "stratum+tcp://";
    static const char *scheme_ssl = "stratum+ssl://";
    static const char *scheme_tls = "stratum+tls://";
    static const char *scheme_plain = "stratum://";

    if (strncmp(url, scheme_ssl, strlen(scheme_ssl)) == 0 ||
        strncmp(url, scheme_tls, strlen(scheme_tls)) == 0) {
        *out_tls = 1; /* STRATUM_TLS_BUNDLED */
        return true;
    }

    if (strncmp(url, scheme_tcp, strlen(scheme_tcp)) == 0 ||
        strncmp(url, scheme_plain, strlen(scheme_plain)) == 0) {
        *out_tls = 0; /* STRATUM_TLS_DISABLED */
        return true;
    }

    /* If backend returns host:port without a scheme, treat as plain SV1/TCP. */
    if (strstr(url, "://") == NULL) {
        *out_tls = 0;
        return true;
    }

    return false;
}

/* Hostname or IPv4 address. */
static bool valid_host(const char *host)
{
    if (host[0] == '\0' || host[0] == '.' || host[0] == '-') {
        return false;
    }
    for (const char *p = host; *p != '\0'; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') {
            return false;
        }
    }
    return true;
}

/* "<address>.<worker>" style stratum user names. */
static bool valid_worker(const char *worker)
{
    size_t len = strlen(worker);
    if (len == 0 || len >= POOL_CONFIG_WORKER_MAX) {
        return false;
    }
    for (const char *p = worker; *p != '\0'; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

/* Printable ASCII without characters that need escaping in stratum JSON. */
static bool valid_password(const char *password)
{
    if (strlen(password) >= POOL_CONFIG_PASSWORD_MAX) {
        return false;
    }
    for (const char *p = password; *p != '\0'; p++) {
        if (*p < 0x20 || *p > 0x7e || *p == '"' || *p == '\\') {
            return false;
        }
    }
    return true;
}

esp_err_t pool_config_parse(const char *response_json, pool_config_t *out)
{
    if (response_json == NULL || response_json[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(response_json);
    if (root == NULL) {
        ESP_LOGW(TAG, "Pool-config response is not valid JSON");
        return ESP_FAIL;
    }

    const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (!cJSON_IsObject(body)) {
        ESP_LOGW(TAG, "Pool-config response does not contain object field 'body'");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(body, "url");
    const cJSON *worker_item = cJSON_GetObjectItemCaseSensitive(body, "worker");
    const cJSON *password_item = cJSON_GetObjectItemCaseSensitive(body, "password");
    const cJSON *power_limit_item = cJSON_GetObjectItemCaseSensitive(body, "psu_power_limit");

    if (cJSON_IsString(url_item) && url_item->valuestring != NULL && url_item->valuestring[0] != '\0') {
        char normalized_url[256] = {0};
        const char *pool_url = normalize_pool_url(url_item->valuestring, normalized_url, sizeof(normalized_url));
        if (pool_url != NULL && parse_pool_url_to_host_port(pool_url, out->host, sizeof(out->host), &out->port)
            && parse_pool_url_to_tls(pool_url, &out->tls) && valid_host(out->host)) {
            out->has_pool = true;
        } else {
            ESP_LOGW(TAG, "Pool-config URL rejected: unsupported format or invalid host");
        }
    } else {
        ESP_LOGW(TAG, "Pool-config body does not contain valid field 'url'");
    }

    if (cJSON_IsString(worker_item) && worker_item->valuestring != NULL) {
        if (valid_worker(worker_item->valuestring)) {
            strlcpy(out->worker, worker_item->valuestring, sizeof(out->worker));
            out->has_worker = true;
        } else {
            ESP_LOGW(TAG, "Pool-config worker rejected: empty, too long or invalid characters");
        }
    }

    if (cJSON_IsString(password_item) && password_item->valuestring != NULL) {
        if (valid_password(password_item->valuestring)) {
            strlcpy(out->password, password_item->valuestring, sizeof(out->password));
            out->has_password = true;
        } else {
            ESP_LOGW(TAG, "Pool-config password rejected: too long or invalid characters");
        }
    }

    if (cJSON_IsNumber(power_limit_item)) {
        double raw = power_limit_item->valuedouble;
        if (raw >= 0.0 && raw <= (double)UINT16_MAX) {
            out->power_limit_w = (uint16_t)raw;
            out->has_power_limit = true;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void pool_config_set_applier(pool_config_applier_t applier)
{
    s_applier = applier;
}

esp_err_t pool_config_apply(const pool_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_applier == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_applier(config);
}
