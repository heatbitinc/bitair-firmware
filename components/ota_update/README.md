# ota_update

Periodic firmware update check and download over HTTPS.

## How it works

- `ota_manager_start()` takes an `ota_config_t`:

```c
typedef struct {
    const char * server_url;     // update server (CONFIG_HEATBIT_OTA_SERVER_URL)
    const char * version_path;   // latest-version request (CONFIG_HEATBIT_OTA_VERSION_PATH)
    const char * download_path;  // image download (CONFIG_HEATBIT_OTA_DOWNLOAD_PATH)
    const char * device_model;   // device model, e.g. "bitair"
    /* Sets TLS and client authentication on every OTA request; CA bundle only if NULL. */
    void (*configure_http)(esp_http_client_config_t * config);
    /* Optional */
    uint32_t check_interval_s;   // how often to check, e.g. 1800
    uint32_t task_stack_size;
    uint32_t task_priority;
} ota_config_t;
```

- The task subscribes to `IP_EVENT` and starts checking after `IP_EVENT_STA_GOT_IP`.
- A check requests `<server_url><version_path>`; if a newer version is available, the image is downloaded from
  `<server_url><download_path>` with `esp_https_ota`. If the server sends an `X-SHA256` header, the SHA-256 of
  the written image is checked before the boot partition is switched.
- `configure_http` is called for both requests. The component itself knows nothing about credentials: the
  application passes a function from `backend_auth` (`backend_auth_ota_http`).

## Example

```c
ota_config_t ota_config = {
    .server_url = CONFIG_HEATBIT_OTA_SERVER_URL,
    .version_path = CONFIG_HEATBIT_OTA_VERSION_PATH,
    .download_path = CONFIG_HEATBIT_OTA_DOWNLOAD_PATH,
    .device_model = "bitair",
    .configure_http = backend_auth_ota_http,
    .check_interval_s = 1800,
};
ota_manager_start(&ota_config);
```

See `ota_update_task` in `main/bitair_app.c`.
