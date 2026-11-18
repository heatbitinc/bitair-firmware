# Trademarks

"Heatbit", "Bitair" and the Heatbit logo are trademarks of Heatbit Inc. The GPLv3 lets you use, change and
distribute this firmware; it does not grant any rights to these trademarks.

## Your own device

You can build a changed firmware and install it on a Bitair you own (see
[doc/installing-your-own-firmware.md](doc/installing-your-own-firmware.md)) without changing anything listed
below.

## Distributing a modified firmware

If you distribute a modified firmware, or a device running it, do not present it as made or endorsed by Heatbit.
You may say that it is based on the Bitair firmware or that it runs on Bitair hardware. Change the names the
firmware uses to identify itself:

| What | Where |
|---|---|
| BLE device name `Heatbit-<id>` and manufacturer data | `HEATBIT_SERVICE_NAME_PREFIX` in `components/ble_provisioning/ble_server.c`, `HEATBIT_DEVICE_MODEL` in `components/heatbit/app_config.h` |
| Wi-Fi hostname `heatbit-<id>` | `WIFI_HOSTNAME_PREFIX` in `components/wifi_manager/wifi_connect.c` |
| Pool user agent `bitair/<asic>/<version>` | `components/stratum/stratum_api.c`, `main/tasks/stratum_v2_task.c` |
| Default pool `solo.pool.heatbit.com` | `STRATUM_URL`, `FALLBACK_STRATUM_URL` in `main/Kconfig.projbuild`, `configs/config-bitair.cvs` |

Bitaxe is a trademark of its respective owner. This firmware is not an official Bitaxe product and is not
endorsed by the Bitaxe project.
