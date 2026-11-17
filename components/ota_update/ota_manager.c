#include "ota_manager.h"
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "ota_manager.h"
#include <string.h>
#include <time.h>

static const char * TAG = "OTA_MGR";

ESP_EVENT_DEFINE_BASE(OTA_EVENTS);

typedef struct
{
    char * server_url;
    char * version_path;
    char * download_path;
    char * device_model;
    void (*configure_http)(esp_http_client_config_t * config);
    uint32_t check_interval_s;
} ota_context_t;

static char response_buffer[128];
static char ota_expected_sha256[65] = {0};
static int response_len = 0;
static TaskHandle_t s_ota_task_handle = NULL;
static ota_runtime_status_t s_ota_runtime_status;

static SemaphoreHandle_t ip_got_semaphore = NULL;

static void ota_runtime_set_check_started(void)
{
    s_ota_runtime_status.last_check_unix_s = (int64_t)time(NULL);
    s_ota_runtime_status.check_status = OTA_CHECK_STATUS_UNKNOWN;
}

static void ota_runtime_set_check_ok_no_update(void)
{
    s_ota_runtime_status.check_status = OTA_CHECK_STATUS_OK;
    s_ota_runtime_status.update_needed_known = true;
    s_ota_runtime_status.update_needed = false;
    s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_IDLE;
    s_ota_runtime_status.update_version[0] = '\0';
}

static void ota_runtime_set_update_found(const char *version)
{
    s_ota_runtime_status.check_status = OTA_CHECK_STATUS_OK;
    s_ota_runtime_status.update_needed_known = true;
    s_ota_runtime_status.update_needed = true;
    if (version != NULL) {
        strlcpy(s_ota_runtime_status.update_version, version, sizeof(s_ota_runtime_status.update_version));
    }
}

static void ota_runtime_set_check_failed(void)
{
    s_ota_runtime_status.check_status = OTA_CHECK_STATUS_FAILED;
    s_ota_runtime_status.update_needed_known = false;
    s_ota_runtime_status.update_needed = false;
    s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_FAILED;
}

static void on_ip_event(void * arg, esp_event_base_t event_base, int32_t event_id, void * event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (ip_got_semaphore != NULL) {
            xSemaphoreGive(ip_got_semaphore);
        }
    }
}

static esp_err_t http_event_handler(esp_http_client_event_handle_t evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        if (evt->user_data) {
            const char * model = (const char *) evt->user_data;
            esp_http_client_set_header(evt->client, "X-MODEL", model);
            ESP_LOGD("OTA_MGR", "Injected X-MODEL: %s into OTA stream", model);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (!evt->user_data && (response_len + evt->data_len < sizeof(response_buffer))) {
            memcpy(response_buffer + response_len, evt->data, evt->data_len);
            response_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        response_buffer[response_len] = '\0';
        break;
    case HTTP_EVENT_DISCONNECTED:
        response_len = 0;
        break;
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "X-SHA256") == 0) {
            strncpy(ota_expected_sha256, evt->header_value, sizeof(ota_expected_sha256) - 1);
            ESP_LOGI("OTA_MGR", "Found X-SHA256 header from server: %s", ota_expected_sha256);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void configure_http(const ota_context_t * ctx, esp_http_client_config_t * config)
{
    if (ctx->configure_http != NULL) {
        ctx->configure_http(config);
    } else {
        config->crt_bundle_attach = esp_crt_bundle_attach;
    }
}

static void perform_ota_check(const ota_context_t * ctx)
{
    ota_runtime_set_check_started();

    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));
    memset(ota_expected_sha256, 0, sizeof(ota_expected_sha256));

    char url_buf[256] = {0};
    snprintf(url_buf, sizeof(url_buf), "%s%s", ctx->server_url, ctx->version_path);
    ESP_LOGI(TAG, "Perform check task");
    esp_http_client_config_t config = {.url = url_buf,
                                       .method = HTTP_METHOD_GET,
                                       .event_handler = http_event_handler};
    configure_http(ctx, &config);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "X-MODEL", ctx->device_model);

    const esp_app_desc_t * app_desc = esp_app_get_description();
    esp_http_client_set_header(client, "X-CURRENT-VERSION", app_desc->version);

    ESP_LOGI(TAG, "Checking updates... Current version: %s", app_desc->version);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);

        if (status_code == 204) {
            ESP_LOGI(TAG, "Device is up to date (Server returned 204).");
            ota_runtime_set_check_ok_no_update();
        } else if (status_code == 200) {
            ota_event_data_t ev_data;
            strlcpy(ev_data.version, response_buffer, sizeof(ev_data.version) - 1);
            esp_event_post(OTA_EVENTS, OTA_EVENT_UPDATE_FOUND, &ev_data, sizeof(ev_data), portMAX_DELAY);
            ESP_LOGI(TAG, "New version available: %s. Starting OTA...", response_buffer);
            ota_runtime_set_update_found(response_buffer);

            char dl_url_buf[256];
            snprintf(dl_url_buf, sizeof(dl_url_buf), "%s%s", ctx->server_url, ctx->download_path);

            esp_http_client_config_t ota_http_config = {
                .url = dl_url_buf,
                .event_handler = http_event_handler,
                .user_data = (void *) ctx->device_model,
            };
            configure_http(ctx, &ota_http_config);

            esp_https_ota_config_t ota_config = {.http_config = &ota_http_config};
            esp_https_ota_handle_t https_ota_handle = NULL;
            const uint32_t ota_progress_log_step_bytes = 128 * 1024;

            esp_err_t ota_begin_err = esp_https_ota_begin(&ota_config, &https_ota_handle);
            if (ota_begin_err == ESP_OK) {
                uint64_t download_start_us = esp_timer_get_time();
                int last_logged_bytes = 0;
                ESP_LOGI(TAG, "OTA download started: %s", dl_url_buf);
                s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_DOWNLOADING;

                esp_event_post(OTA_EVENTS, OTA_EVENT_START_DOWNLOAD, NULL, 0, portMAX_DELAY);
                esp_err_t ota_err = ESP_ERR_HTTPS_OTA_IN_PROGRESS;
                while (ota_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                    ota_err = esp_https_ota_perform(https_ota_handle);

                    int downloaded_bytes = esp_https_ota_get_image_len_read(https_ota_handle);
                    if (downloaded_bytes - last_logged_bytes >= (int) ota_progress_log_step_bytes) {
                        ESP_LOGI(TAG, "OTA download progress: %d bytes", downloaded_bytes);
                        last_logged_bytes = downloaded_bytes;
                    }
                }

                if (ota_err == ESP_OK) {
                    int downloaded_bytes = esp_https_ota_get_image_len_read(https_ota_handle);
                    uint64_t download_elapsed_ms = (esp_timer_get_time() - download_start_us) / 1000;
                    ESP_LOGI(TAG, "OTA download finished: %d bytes in %llu ms", downloaded_bytes,
                             (unsigned long long) download_elapsed_ms);
                    esp_event_post(OTA_EVENTS, OTA_EVENT_FINISH_DOWNLOAD, NULL, 0, portMAX_DELAY);
                    s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_IDLE;

                    bool custom_sha_valid = true;
                    if (strlen(ota_expected_sha256) > 0) {
                        int image_len = esp_https_ota_get_image_len_read(https_ota_handle);
                        const esp_partition_t * update_partition = esp_ota_get_next_update_partition(NULL);

                        if (image_len > 0 && update_partition != NULL) {
                            mbedtls_sha256_context sha_ctx;
                            mbedtls_sha256_init(&sha_ctx);
                            mbedtls_sha256_starts(&sha_ctx, 0); // 0 = SHA-256

                            uint8_t read_buf[512];
                            int offset = 0;

                            while (offset < image_len) {
                                int bytes_to_read =
                                    (image_len - offset > sizeof(read_buf)) ? sizeof(read_buf) : (image_len - offset);

                                if (esp_partition_read(update_partition, offset, read_buf, bytes_to_read) != ESP_OK) {
                                    ESP_LOGE(TAG, "Flash read error at offset %d", offset);
                                    custom_sha_valid = false;
                                    break;
                                }

                                mbedtls_sha256_update(&sha_ctx, read_buf, bytes_to_read);
                                offset += bytes_to_read;
                            }

                            uint8_t calc_sha256[32];
                            mbedtls_sha256_finish(&sha_ctx, calc_sha256);
                            mbedtls_sha256_free(&sha_ctx);

                            char calc_sha256_str[65];
                            for (int i = 0; i < 32; i++) {
                                sprintf(&calc_sha256_str[i * 2], "%02x", calc_sha256[i]);
                            }
                            calc_sha256_str[64] = '\0';

                            if (strcasecmp(ota_expected_sha256, calc_sha256_str) != 0) {
                                ESP_LOGE(TAG, "SHA256 mismatch! Expected: %s, Got: %s", ota_expected_sha256, calc_sha256_str);
                                custom_sha_valid = false;
                            } else {
                                ESP_LOGI(TAG, "SHA256 checksum verified successfully!");
                            }
                        }
                    }

                    if (!custom_sha_valid) {
                        esp_event_post(OTA_EVENTS, OTA_EVENT_CHECK_FW_FAILED, NULL, 0, portMAX_DELAY);
                        s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_FAILED;
                        esp_https_ota_abort(https_ota_handle);
                    } else {
                        if (esp_https_ota_finish(https_ota_handle) == ESP_OK) {
                            ESP_LOGI(TAG, "OTA successful! Ready for reboot.");
                            esp_event_post(OTA_EVENTS, OTA_EVENT_READY_FOR_REBOOT, NULL, 0, portMAX_DELAY);
                            s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_READY_FOR_REBOOT;
                        } else {
                            ESP_LOGE(TAG, "OTA finish/validation failed!");
                            esp_event_post(OTA_EVENTS, OTA_EVENT_CHECK_FW_FAILED, NULL, 0, portMAX_DELAY);
                            s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_FAILED;
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "OTA download failed during perform: %s", esp_err_to_name(ota_err));
                    s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_FAILED;
                    esp_https_ota_abort(https_ota_handle);
                }

                /*
                  uint8_t calc_sha256[32];
                  esp_partition_get_sha256(esp_ota_get_next_update_partition(esp_ota_get_running_partition()), calc_sha256);

                  char calc_sha256_str[65];
                  for (int i = 0; i < 32; i++)
                      sprintf(&calc_sha256_str[i * 2], "%02x", calc_sha256[i]);

                  if (strlen(ota_expected_sha256) > 0 && strcmp(ota_expected_sha256, calc_sha256_str) != 0) {
                      ESP_LOGE(TAG, "SHA256 mismatch! Expected: %s, Got: %s", ota_expected_sha256, calc_sha256_str);
                      esp_event_post(OTA_EVENTS, OTA_EVENT_CHECK_FW_FAILED, NULL, 0, portMAX_DELAY);
                      esp_https_ota_abort(https_ota_handle);
                  } else {
                      esp_event_post(OTA_EVENTS, OTA_EVENT_FINISH_DOWNLOAD, NULL, 0, portMAX_DELAY);
                      ESP_LOGI(TAG, "SHA256 validation passed. Finalizing OTA and restart...");
                      if (esp_https_ota_finish(https_ota_handle) == ESP_OK) {
                          esp_event_post(OTA_EVENTS, OTA_EVENT_READY_FOR_REBOOT, NULL, 0, portMAX_DELAY);
                      }

                  }
              } else {
                  ESP_LOGE(TAG, "OTA performance failed.");
                  esp_https_ota_abort(https_ota_handle);
              }
              */
                        } else {
                                ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ota_begin_err));
                                s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_FAILED;
            }
        } else {
            ESP_LOGE(TAG, "Server returned unexpected status: %d", status_code);
            ota_runtime_set_check_failed();
        }
    } else {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        ota_runtime_set_check_failed();
    }

    esp_http_client_cleanup(client);
}

static void ota_task(void * pvParameters)
{
    ota_context_t * ctx = (ota_context_t *) pvParameters;

    ESP_LOGI(TAG, "OTA task waiting for first IP address...");
    xSemaphoreTake(ip_got_semaphore, portMAX_DELAY);

    while (1) {
        esp_netif_t * netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                perform_ota_check(ctx);
            } else {
                ESP_LOGW(TAG, "Skipping OTA check: no Wi-Fi IP yet");
            }
        } else {
            ESP_LOGW(TAG, "Skipping OTA check: WIFI_STA_DEF netif is unavailable");
        }

        ESP_LOGI(TAG, "Next OTA check in %u seconds", (unsigned)ctx->check_interval_s);
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ctx->check_interval_s * 1000));
        if (notified > 0) {
            ESP_LOGI(TAG, "OTA immediate check requested");
        }
    }
}

esp_err_t ota_manager_trigger_check(void)
{
    if (s_ota_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(s_ota_task_handle);
    return ESP_OK;
}

esp_err_t ota_manager_start(const ota_config_t * config)
{
    if (!config || !config->server_url || !config->version_path || !config->download_path || !config->device_model) {
        ESP_LOGE(TAG, "Invalid configuration");

        return ESP_ERR_INVALID_ARG;
    }

    if (ip_got_semaphore == NULL) {
        ip_got_semaphore = xSemaphoreCreateBinary();
        if (!ip_got_semaphore)
            return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t * netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "IP already acquired upon start. Unblocking OTA task.");
            xSemaphoreGive(ip_got_semaphore);
        }
    }
    ESP_LOGI(TAG, "Got configuration: %s %s", config->server_url, config->device_model);
    ota_context_t * ctx = calloc(1, sizeof(ota_context_t));
    if (!ctx)
        return ESP_ERR_NO_MEM;

    ctx->server_url = strdup(config->server_url);
    ctx->version_path = strdup(config->version_path);
    ctx->download_path = strdup(config->download_path);
    ctx->device_model = strdup(config->device_model);
    ctx->configure_http = config->configure_http;
    ctx->check_interval_s = config->check_interval_s > 0 ? config->check_interval_s : 3600;
    if (!ctx->server_url || !ctx->version_path || !ctx->download_path || !ctx->device_model) {
        ESP_LOGE(TAG, "Failed to allocate memory for config strings");
        free(ctx->server_url);
        free(ctx->version_path);
        free(ctx->download_path);
        free(ctx->device_model);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    uint32_t stack_size = config->task_stack_size > 0 ? config->task_stack_size : 12288;
    uint32_t priority = config->task_priority > 0 ? config->task_priority : 5;

    ESP_LOGI(TAG, "Context created");

    memset(&s_ota_runtime_status, 0, sizeof(s_ota_runtime_status));
    s_ota_runtime_status.check_status = OTA_CHECK_STATUS_UNKNOWN;
    s_ota_runtime_status.process_status = OTA_PROCESS_STATUS_IDLE;
    s_ota_runtime_status.update_version[0] = '\0';

    BaseType_t ret = xTaskCreate(ota_task, "ota_mgr_task", stack_size, (void *) ctx, priority, &s_ota_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Creation OTA task failed");
        s_ota_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ota_manager_get_runtime_status(ota_runtime_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_status = s_ota_runtime_status;
    return ESP_OK;
}
