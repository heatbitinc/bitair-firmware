# Bitair firmware architecture

Bitair is an air purifier with a built-in solo Bitcoin miner: a BM1370 ASIC controlled by an ESP32-S3. Its firmware is a fork of
[ESP-Miner](https://github.com/bitaxeorg/ESP-Miner): the mining core (stratum, ASIC drivers, power and thermal
control) comes from upstream, Bitair adds its board peripherals, Wi-Fi/BLE provisioning and the Heatbit cloud.
The upstream web UI (AxeOS), HTTP server, display and BAP are removed.

## Layout

```
main/                    upstream startup sequence and mining tasks, plus the Bitair application layer
components/
  asic/ stratum/ stratum_v2/ libsecp256k1/    upstream mining core
  ota_update/            firmware update client
  device_state/          device state store with change listeners, telemetry snapshot
  device_identity/       device UUID, worker_id, device client certificate
  backend_auth/          how each backend channel authenticates (mutual TLS)
  bitair_hw/             indication LED, PWM power/fan input, TPM-300A air quality sensor
  wifi_manager/          Wi-Fi station: connect, retry, factory-network fallback, reachability check
  ble_provisioning/      BLE GATT service for Wi-Fi provisioning from the mobile app
  heatbit/               Heatbit cloud: metrics, WebSocket, MQTT, pool settings, time sync
```

Dependencies only point down; nothing in `components/` includes headers from `main/`:

| Component | Depends on |
|---|---|
| `device_state` | NVS |
| `device_identity` | `device_state` |
| `backend_auth` | `device_identity` |
| `bitair_hw` | `device_state` |
| `wifi_manager` | `device_state` |
| `heatbit` | `backend_auth`, `wifi_manager`, `bitair_hw`, `device_identity`, `device_state`, `ota_update` |
| `ble_provisioning` | `heatbit`, `wifi_manager`, `bitair_hw`, `device_identity`, `device_state` |
| `main` | everything above |

Where a component needs something from the application, `main` registers it at startup:

| Hook | Registered by | Used by |
|---|---|---|
| state listeners (`state_service_add_listener`) | `main/state_listeners.c`, `ws_backend_service` | LED, mining gate, backend status |
| miner snapshot (`miner_status_set_provider`) | `main/miner_status_provider.c` | MQTT and metrics telemetry |
| pool settings (`pool_config_set_applier`) | `main/pool_config_applier.c` | backend pool-config |

## Startup

`app_main` (`main/main.c`) keeps the upstream order and calls the Bitair layer (`main/bitair_app.c`) at fixed
points:

1. `bitair_app_init_early` — log level, event loop and OTA events, LED, system status.
2. I2C, ASIC reset held low, ADC, NVS (upstream).
3. `bitair_app_init_state` — device state and listeners, telemetry and pool hooks, device ID, device certificate.
4. `device_config_init` (upstream), then `bitair_app_init_board` — board defaults, power management binding.
5. Self-test, system and peripheral init (upstream).
6. `bitair_app_start_services` — PWM input, fan, Wi-Fi, BLE, certificate renewal, air sensors, time sync, OTA,
   metrics, WebSocket, MQTT, status reporter.
7. Power management task (upstream), then wait for Wi-Fi.
8. `bitair_app_start_mining` — mining worker tasks and the startup decision from the PWM input.
9. `bitair_app_after_mining_start` — V/F calibration (development builds only).
10. Protocol coordinator and self-test (upstream).

## Device state

`device_state/state_service` holds what the user and the backend control: power (`power_pin`), fan mode, work
mode, saved Wi-Fi credentials, sensor values. It only stores; reactions are listeners:

- `main/state_listeners.c`: network status into `GlobalState`, LED, mining gate (mining runs while powered on
  in the `heater` work mode; in `air_purifier` mode only the fan runs);
- `ws_backend_service_state_listener`: publishes power/fan/work mode to the backend.

The PWM input from the display is the source of power and fan mode. The backend can change them too.

## Mining power and temperature

Only the power management task (`main/tasks/power_management_task.c`) starts and stops the ASIC. It starts
paused and does nothing before `app_main` reports that the network is up.

- Normal stop: frequency ramp down, then the core regulator off.
- Faults (overheat, temperature sensor not answering, TPS546 fault): regulator off at once.
- Thermal governor (`main/thermal/thermal_governor.c`): lowers the frequency in 25 MHz steps while the chip is
  at or above 70 °C (regulator 95 °C) and raises it again at or below 65 °C (85 °C), by partial ramps without
  stopping mining. Emergency stop at 75 °C chip / 105 °C regulator.
- Temperature sensor fault: ten failed reads in a row while mining stop it; it restarts after the sensors have
  answered for 15 s.
- Frequency ramp step and delay are Kconfig options; Bitair uses 25 MHz every 50 ms.

## Cloud

| Channel | Module | Purpose |
|---|---|---|
| HTTPS metrics, pool-config | `heatbit/metrics_service`, `heatbit/pool_config` | telemetry, pool settings (validated) |
| WebSocket | `heatbit/ws_backend_service` | live status, remote commands |
| MQTT | `heatbit/mqtt_service` | telemetry and commands |
| HTTPS OTA | `ota_update` | firmware updates |
| HTTPS enrollment | `device_identity/device_cert` | renewal of the device client certificate |

Every channel takes its TLS and credentials from `backend_auth`: the server is verified against the ESP-IDF CA
bundle and the device presents its client certificate (mutual TLS). The WebSocket handshake also carries an
`Auth` header with a token built from the device UUID and worker ID. The services only add their own
identification. Backend hosts and paths are Kconfig options without defaults, set in
`sdkconfig.defaults.heatbit` (see `sdkconfig.defaults.heatbit.example`).

## Device identity

Each device is provisioned at manufacturing: the device UUID, the worker ID and the first client certificate
(an EC P-256 key with a certificate for `<uuid>.<CONFIG_HEATBIT_DEVICES_DNS_SUFFIX>`) are written into NVS. The
firmware only reads the identifiers. With `CONFIG_HEATBIT_ENROLL_ENABLED` it renews the certificate before it
expires: a new key is generated on the chip and the CSR is authenticated with the current certificate. Two NVS
slots keep the old pair until the new one is stored. A device without identity still mines with the pool
settings in NVS, but cannot use the cloud.

## Configuration

| File | Content |
|---|---|
| `sdkconfig.defaults` | settings that differ from the ESP-IDF and component defaults |
| `sdkconfig.defaults.heatbit` | backend endpoints (not in git; picked up automatically if present) |
| `certs/ca_root.pem` (not in git) | root CA of the enrollment server, needed for certificate renewal |
| `configs/config-bitair.cvs` | NVS image with the default settings (`config.bin`) |
| `partitions.csv` | flash layout; NVS at `0x9000` holds the device identity and must never be erased |

## Tests

Unity tests live in `components/*/test` and run on the ESP32-S3: `test-ci` runs them all and exits, `test` runs
them and opens the interactive menu. See [doc/unit_testing.md](doc/unit_testing.md).
