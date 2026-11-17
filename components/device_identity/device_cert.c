#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "device_cert.h"
#include "device_id.h"
#include "esp_wifi.h"

static const char *TAG = "device_cert";

#define NVS_NS "dev_cert"
#define NVS_KEY_UUID "uuid"
#define NVS_KEY_SLOT "slot"
static const char *const NVS_KEY_KEY[2] = {"key", "key1"};
static const char *const NVS_KEY_CRT[2] = {"crt", "crt1"};

#define KEY_PEM_MAX 512
#define CRT_PEM_MAX 4000 /* NVS string limit */
#define CSR_PEM_MAX 1024
#define SAN_MAX 128

#define SECONDS_PER_DAY 86400

typedef struct {
    char *key_pem;
    char *crt_pem;
    time_t not_before;
    time_t not_after;
} cert_pair_t;

static char s_uuid[DEVICE_UUID4_STR_LEN];
static uint8_t s_slot;
/*
 * The published pair is replaced as a whole by swapping one pointer, so a
 * reader always sees a key and the certificate that belongs to it. Published
 * pairs are never freed: TLS clients may still hold their buffers.
 */
static const cert_pair_t *s_current;

static const cert_pair_t *current_pair(void)
{
    return __atomic_load_n(&s_current, __ATOMIC_ACQUIRE);
}


static int hw_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static void build_san(char out[SAN_MAX])
{
    snprintf(out, SAN_MAX, "%s.%s", s_uuid, CONFIG_HEATBIT_DEVICES_DNS_SUFFIX);
}

/* UTC calendar time to Unix time, independent of the TZ setting. */
static time_t x509_time_to_epoch(const mbedtls_x509_time *t)
{
    int y = t->year;
    int m = t->mon;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t->day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (time_t)(days * SECONDS_PER_DAY + t->hour * 3600 + t->min * 60 + t->sec);
}

static char *nvs_read_str_alloc(nvs_handle_t nvs, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(nvs, key, NULL, &len) != ESP_OK || len == 0) {
        return NULL;
    }
    char *buf = malloc(len);
    if (buf == NULL) {
        return NULL;
    }
    if (nvs_get_str(nvs, key, buf, &len) != ESP_OK) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Checks that the chain parses, belongs to the key and names our SAN; fills the validity period. */
static esp_err_t check_pair(cert_pair_t *pair)
{
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&key);
    esp_err_t result = ESP_FAIL;

    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pair->crt_pem, strlen(pair->crt_pem) + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Certificate parse failed: -0x%04x", -ret);
        goto out;
    }
    ret = mbedtls_pk_parse_key(&key, (const unsigned char *)pair->key_pem, strlen(pair->key_pem) + 1, NULL, 0,
                               hw_rng, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Key parse failed: -0x%04x", -ret);
        goto out;
    }
    ret = mbedtls_pk_check_pair(&crt.pk, &key, hw_rng, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Certificate does not match device key: -0x%04x", -ret);
        goto out;
    }

    char san[SAN_MAX];
    build_san(san);
    bool san_ok = false;
    for (const mbedtls_x509_sequence *cur = &crt.subject_alt_names; cur != NULL && cur->buf.p != NULL; cur = cur->next) {
        mbedtls_x509_subject_alternative_name name;
        if (mbedtls_x509_parse_subject_alt_name(&cur->buf, &name) != 0) {
            continue;
        }
        if (name.type == MBEDTLS_X509_SAN_DNS_NAME && name.san.unstructured_name.len == strlen(san)
            && memcmp(name.san.unstructured_name.p, san, strlen(san)) == 0) {
            san_ok = true;
        }
        mbedtls_x509_free_subject_alt_name(&name);
    }
    if (!san_ok) {
        ESP_LOGE(TAG, "Certificate SAN does not match %s", san);
        goto out;
    }

    pair->not_before = x509_time_to_epoch(&crt.valid_from);
    pair->not_after = x509_time_to_epoch(&crt.valid_to);
    char subject[160];
    mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
    ESP_LOGI(TAG, "Device certificate: %s, valid until %04d-%02d-%02d", subject, crt.valid_to.year, crt.valid_to.mon,
             crt.valid_to.day);
    result = ESP_OK;

out:
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&crt);
    return result;
}

/* Takes ownership of the pair's buffers. */
static esp_err_t publish_pair(const cert_pair_t *pair)
{
    cert_pair_t *published = malloc(sizeof(*published));
    if (published == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *published = *pair;
    __atomic_store_n(&s_current, published, __ATOMIC_RELEASE);
    return ESP_OK;
}

static void erase_identity(nvs_handle_t nvs)
{
    nvs_erase_key(nvs, NVS_KEY_UUID);
    nvs_erase_key(nvs, NVS_KEY_SLOT);
    for (int i = 0; i < 2; i++) {
        nvs_erase_key(nvs, NVS_KEY_KEY[i]);
        nvs_erase_key(nvs, NVS_KEY_CRT[i]);
    }
    nvs_commit(nvs);
}

esp_err_t device_cert_init(const char *device_uuid)
{
    if (device_uuid == NULL || strlen(device_uuid) != DEVICE_UUID4_STR_LEN - 1) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_uuid, device_uuid, sizeof(s_uuid));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char stored_uuid[DEVICE_UUID4_STR_LEN] = {0};
    size_t len = sizeof(stored_uuid);
    if (nvs_get_str(nvs, NVS_KEY_UUID, stored_uuid, &len) == ESP_OK && strcmp(stored_uuid, s_uuid) != 0) {
        /* The stored pair was issued for another device UUID. */
        ESP_LOGW(TAG, "Device UUID changed, discarding old certificate");
        erase_identity(nvs);
    }

    uint8_t slot = 0;
    if (nvs_get_u8(nvs, NVS_KEY_SLOT, &slot) != ESP_OK || slot > 1) {
        slot = 0;
    }
    s_slot = slot;

    cert_pair_t pair = {
        .key_pem = nvs_read_str_alloc(nvs, NVS_KEY_KEY[slot]),
        .crt_pem = nvs_read_str_alloc(nvs, NVS_KEY_CRT[slot]),
    };
    nvs_close(nvs);

    if (pair.key_pem != NULL && pair.crt_pem != NULL && check_pair(&pair) == ESP_OK
        && publish_pair(&pair) == ESP_OK) {
        return ESP_OK;
    }

    free(pair.key_pem);
    free(pair.crt_pem);
    ESP_LOGI(TAG, "Device is not enrolled");
    return ESP_OK;
}

bool device_cert_is_enrolled(void)
{
    return current_pair() != NULL;
}

bool device_cert_wait_enrolled(TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    while (current_pair() == NULL) {
        if (xTaskGetTickCount() - start >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return true;
}

const char *device_cert_get_cert_pem(void)
{
    const cert_pair_t *pair = current_pair();
    return pair != NULL ? pair->crt_pem : NULL;
}

const char *device_cert_get_key_pem(void)
{
    const cert_pair_t *pair = current_pair();
    return pair != NULL ? pair->key_pem : NULL;
}

time_t device_cert_get_not_after(void)
{
    const cert_pair_t *pair = current_pair();
    return pair != NULL ? pair->not_after : 0;
}

void device_cert_apply_client_cert(esp_http_client_config_t *config)
{
    const cert_pair_t *pair = current_pair();
    if (config == NULL || pair == NULL) {
        return;
    }
    config->client_key_pem = pair->key_pem;
    config->client_cert_pem = pair->crt_pem;
}

#if CONFIG_HEATBIT_ENROLL_ENABLED

#define RESPONSE_MAX 6144
#define ENROLL_TASK_STACK 8192
#define ENROLL_HTTP_TIMEOUT_MS 30000
#define ENROLL_RETRY_MIN_S 30
#define ENROLL_RETRY_MAX_S 1800
#define ENROLL_REJECTED_RETRY_S 3600
#define RENEWAL_CHECK_PERIOD_S (12 * 3600)
#define TIME_UNSET_RECHECK_S 600
/* Earliest plausible wall-clock time (2025-01-01); before SNTP sync the clock starts at 1970. */
#define MIN_VALID_EPOCH 1735689600

/* Renewal window as agreed with the backend: 90 days before expiry, 30 days for certificates of a year or less. */
#define RENEW_BEFORE_LONG_DAYS 90
#define RENEW_BEFORE_SHORT_DAYS 30
#define SHORT_LIFETIME_DAYS 365

extern const char ca_root_pem_start[] asm("_binary_ca_root_pem_start");

static bool s_task_started;

/* Same check as wifi_connect's wifi_is_connected(), without depending on it. */
static bool wifi_associated(void)
{
    wifi_ap_record_t ap_info;
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

static char *generate_key(void)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    char *key_pem = calloc(1, KEY_PEM_MAX);
    int ret = key_pem == NULL ? -1 : mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret == 0) {
        ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), hw_rng, NULL);
    }
    if (ret == 0) {
        ret = mbedtls_pk_write_key_pem(&pk, (unsigned char *)key_pem, KEY_PEM_MAX);
    }
    mbedtls_pk_free(&pk);
    if (ret != 0) {
        ESP_LOGE(TAG, "Key generation failed: -0x%04x", -ret);
        free(key_pem);
        return NULL;
    }
    return key_pem;
}

static char *build_csr(const char *key_pem)
{
    char san[SAN_MAX];
    char subject[SAN_MAX + 32];
    build_san(san);
    snprintf(subject, sizeof(subject), "CN=%s,O=Heatbit,OU=Devices", san);

    mbedtls_pk_context pk;
    mbedtls_x509write_csr csr;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr_init(&csr);
    char *csr_pem = calloc(1, CSR_PEM_MAX);

    mbedtls_x509_san_list san_list = {
        .node = {
            .type = MBEDTLS_X509_SAN_DNS_NAME,
            .san.unstructured_name = {.p = (unsigned char *)san, .len = strlen(san)},
        },
        .next = NULL,
    };

    int ret = csr_pem == NULL ? -1
                              : mbedtls_pk_parse_key(&pk, (const unsigned char *)key_pem, strlen(key_pem) + 1, NULL, 0,
                                                     hw_rng, NULL);
    if (ret == 0) {
        mbedtls_x509write_csr_set_key(&csr, &pk);
        mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
        ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
    }
    if (ret == 0) {
        ret = mbedtls_x509write_csr_set_subject_alternative_name(&csr, &san_list);
    }
    if (ret == 0) {
        ret = mbedtls_x509write_csr_pem(&csr, (unsigned char *)csr_pem, CSR_PEM_MAX, hw_rng, NULL);
    }

    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    if (ret != 0) {
        ESP_LOGE(TAG, "CSR generation failed: -0x%04x", -ret);
        free(csr_pem);
        return NULL;
    }
    return csr_pem;
}

/* POSTs the CSR. Returns the HTTP status, or -1 on transport errors; *out_body is malloc'ed. */
static int post_enroll(const char *body, char **out_body)
{
    *out_body = NULL;
    esp_http_client_config_t cfg = {
        .url = CONFIG_HEATBIT_ENROLL_URL,
        .method = HTTP_METHOD_POST,
        .cert_pem = ca_root_pem_start,
        .timeout_ms = ENROLL_HTTP_TIMEOUT_MS,
    };
    /* Renewal: authenticate with the current device certificate. */
    device_cert_apply_client_cert(&cfg);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    int status = -1;
    size_t body_len = strlen(body);
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK && esp_http_client_write(client, body, body_len) == (int)body_len
        && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        char *resp = calloc(1, RESPONSE_MAX);
        if (resp != NULL) {
            int total = 0;
            int n;
            while (total < RESPONSE_MAX - 1
                   && (n = esp_http_client_read(client, resp + total, RESPONSE_MAX - 1 - total)) > 0) {
                total += n;
            }
            *out_body = resp;
        }
    } else {
        ESP_LOGW(TAG, "Enrollment request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/* Leaf certificate followed by the issuing CA, as needed for the TLS client chain. */
static char *chain_from_response(const char *resp)
{
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        return NULL;
    }
    const cJSON *crt = cJSON_GetObjectItemCaseSensitive(root, "crt");
    const cJSON *ca = cJSON_GetObjectItemCaseSensitive(root, "ca");
    char *chain = NULL;
    if (cJSON_IsString(crt) && crt->valuestring != NULL) {
        const char *ca_pem = (cJSON_IsString(ca) && ca->valuestring != NULL) ? ca->valuestring : "";
        size_t crt_len = strlen(crt->valuestring);
        bool need_nl = crt_len > 0 && crt->valuestring[crt_len - 1] != '\n';
        size_t len = crt_len + (need_nl ? 1 : 0) + strlen(ca_pem) + 1;
        if (len <= CRT_PEM_MAX) {
            chain = malloc(len);
            if (chain != NULL) {
                snprintf(chain, len, "%s%s%s", crt->valuestring, need_nl ? "\n" : "", ca_pem);
            }
        } else {
            ESP_LOGE(TAG, "Certificate chain too large: %u bytes", (unsigned)len);
        }
    }
    cJSON_Delete(root);
    return chain;
}

/* Writes the pair into the inactive slot, then switches the active slot. */
static esp_err_t store_pair(const cert_pair_t *pair)
{
    uint8_t slot = current_pair() != NULL ? (uint8_t)(1 - s_slot) : s_slot;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, NVS_KEY_KEY[slot], pair->key_pem);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_CRT[slot], pair->crt_pem);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_UUID, s_uuid);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, NVS_KEY_SLOT, slot);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_slot = slot;
    }
    return err;
}

static bool renewal_due(time_t now)
{
    const cert_pair_t *current = current_pair();
    if (current == NULL) {
        return true;
    }
    time_t lifetime = current->not_after - current->not_before;
    int before_days = lifetime > (time_t)SHORT_LIFETIME_DAYS * SECONDS_PER_DAY ? RENEW_BEFORE_LONG_DAYS
                                                                                : RENEW_BEFORE_SHORT_DAYS;
    return now >= current->not_after - (time_t)before_days * SECONDS_PER_DAY;
}

/* One enrollment attempt. Returns the number of seconds to wait before the next one, 0 on success. */
static uint32_t enroll_once(uint32_t backoff_s)
{
    cert_pair_t pair = {.key_pem = generate_key()};
    if (pair.key_pem == NULL) {
        return backoff_s;
    }
    char *csr_pem = build_csr(pair.key_pem);
    if (csr_pem == NULL) {
        free(pair.key_pem);
        return backoff_s;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "device_uuid", s_uuid);
    cJSON_AddStringToObject(req, "device_type", CONFIG_HEATBIT_DEVICE_TYPE);
    cJSON_AddStringToObject(req, "csr", csr_pem);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(csr_pem);
    if (body == NULL) {
        free(pair.key_pem);
        return backoff_s;
    }

    char *resp = NULL;
    int status = post_enroll(body, &resp);
    free(body);

    uint32_t retry_s = backoff_s;
    if (status == 200 && resp != NULL) {
        pair.crt_pem = chain_from_response(resp);
        if (pair.crt_pem != NULL && check_pair(&pair) == ESP_OK && store_pair(&pair) == ESP_OK) {
            bool renewal = current_pair() != NULL;
            if (publish_pair(&pair) == ESP_OK) {
                ESP_LOGI(TAG, "%s complete", renewal ? "Renewal" : "Enrollment");
                pair.key_pem = NULL;
                pair.crt_pem = NULL;
                retry_s = 0;
            } else {
                /* Stored in NVS already: it is loaded on the next boot. */
                ESP_LOGE(TAG, "Out of memory publishing the new certificate");
            }
        } else {
            ESP_LOGE(TAG, "Enrollment response rejected");
        }
    } else if (status >= 400 && status < 500) {
        /* The server rejected the request itself: retrying quickly will not help. */
        cJSON *err_json = resp != NULL ? cJSON_Parse(resp) : NULL;
        const cJSON *detail = err_json != NULL ? cJSON_GetObjectItemCaseSensitive(err_json, "detail") : NULL;
        ESP_LOGW(TAG, "Enrollment rejected: HTTP %d %s", status,
                 cJSON_IsString(detail) ? detail->valuestring : "");
        cJSON_Delete(err_json);
        retry_s = ENROLL_REJECTED_RETRY_S;
    } else {
        ESP_LOGW(TAG, "Enrollment failed: HTTP %d", status);
    }

    free(resp);
    free(pair.key_pem);
    free(pair.crt_pem);
    return retry_s;
}

static void enroll_task(void *arg)
{
    (void)arg;
    uint32_t backoff_s = ENROLL_RETRY_MIN_S;
    while (true) {
        if (!wifi_associated()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        uint32_t wait_s;
        time_t now = time(NULL);
        const cert_pair_t *current = current_pair();
        if (current != NULL && now < MIN_VALID_EPOCH) {
            /* Renewal needs wall-clock time; wait for SNTP. */
            wait_s = TIME_UNSET_RECHECK_S;
        } else if (!renewal_due(now)) {
            wait_s = RENEWAL_CHECK_PERIOD_S;
        } else {
            if (current != NULL) {
                ESP_LOGI(TAG, "Certificate expires in %lld days, renewing",
                         (long long)((current->not_after - now) / SECONDS_PER_DAY));
            }
            wait_s = enroll_once(backoff_s);
            if (wait_s == 0) {
                backoff_s = ENROLL_RETRY_MIN_S;
                continue;
            }
            ESP_LOGI(TAG, "Next enrollment attempt in %lu s", (unsigned long)wait_s);
            if (wait_s == backoff_s) {
                backoff_s = backoff_s * 2 > ENROLL_RETRY_MAX_S ? ENROLL_RETRY_MAX_S : backoff_s * 2;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait_s * 1000U));
    }
}

esp_err_t device_cert_start_enrollment(void)
{
    if (s_task_started) {
        return ESP_OK;
    }
    if (s_uuid[0] == '\0' || current_pair() == NULL) {
        /* Only renewal: the first certificate is issued at manufacturing. */
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(enroll_task, "device_enroll", ENROLL_TASK_STACK, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_task_started = true;
    return ESP_OK;
}

#else /* !CONFIG_HEATBIT_ENROLL_ENABLED */

esp_err_t device_cert_start_enrollment(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
