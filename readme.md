# Bitair firmware

Firmware for the [Heatbit Bitair](https://heatbit.com/products/bitair): a quiet HEPA air purifier with a
built-in 1.2 TH/s solo Bitcoin miner. A Bitmain BM1370 ASIC mines and an ESP32-S3 runs this firmware; the heat
from the ASIC is used by the purifier.

The firmware is a fork of [ESP-Miner](https://github.com/bitaxeorg/ESP-Miner), the open-source firmware of the
[Bitaxe](https://github.com/bitaxeorg/bitaxe). The main board design is published separately:
[heatbitinc/bitair-mainboard](https://github.com/heatbitinc/bitair-mainboard) (based on bitaxeGamma).

What Bitair adds on top of ESP-Miner:

- power and fan mode from the device display (PWM input), indication LED, air quality and climate sensors;
- Wi-Fi provisioning over BLE from the Heatbit app;
- the Heatbit cloud: telemetry, remote control, pool settings, OTA updates, all over mutual TLS;
- temperature-driven frequency control and fault handling for continuous quiet operation inside the purifier.

The ESP-Miner web UI (AxeOS), HTTP API and display support are not part of this firmware.

See [ARCHITECTURE.md](ARCHITECTURE.md) for how the firmware is organised and [AGENTS.md](AGENTS.md) for build,
flash and test commands.

## Building

Requirements: [ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/) and
the git submodules.

```bash
git clone --recursive <repository>
cd <repository>
idf.py build merge_bins
```

`merge_bins` writes `build/bitair-miner_full.bin` (bootloader, partition table and application).

The Heatbit cloud endpoints are not part of this repository. Without them the firmware builds and mines with the
pool settings stored on the device, but does not connect to the cloud. To point a build at a backend, copy
`sdkconfig.defaults.heatbit.example` to `sdkconfig.defaults.heatbit` and fill it in.

Production builds also add `sdkconfig.defaults.security` (Secure Boot, Flash Encryption, NVS encryption). Read
the warning at the top of that file before using it.

## Device identity

Each Bitair is provisioned at manufacturing: its UUID, worker ID and client certificate are written into NVS.
The firmware uses the certificate for every connection to the cloud and renews it before it expires. Never erase
the NVS partition (`0x9000`) or the whole flash of a real device: the identity cannot be recreated.

## Installing your own firmware

You can build this firmware, change it and install it on your own Bitair: see
[doc/installing-your-own-firmware.md](doc/installing-your-own-firmware.md). On a provisioned device write only
the application, so NVS and the device identity stay intact.

Programming a blank main board (power, connectors, download mode, merged image) is described in
[FLASHING_BITAIR_MAINBOARD.md](FLASHING_BITAIR_MAINBOARD.md).

## Privacy

The device sends to the Heatbit cloud:

- device identifiers (UUID), firmware version and security state (Secure Boot, Flash Encryption, certificate
  expiry);
- mining statistics: hashrate, shares, pool difficulty, ASIC frequency, voltage, temperatures and power;
- device state: power, fan and work mode, sensor readings (temperature, humidity, CO2, TVOC);
- network state: Wi-Fi SSID, signal strength and the local IP address.

The Wi-Fi password and the pool password are not sent. Device logs stay on the serial port.

## Wi-Fi routers

Some Wi-Fi routers block mining traffic, for example ASUS routers (AiProtection, IoT protection) and some
TP-Link models. If the device connects but does not mine, check those settings and the pool host and port.

## License

Copyright (C) 2025-2026 Heatbit Inc. Based on ESP-Miner, copyright the ESP-Miner contributors.

This firmware is licensed under the GNU General Public License version 3, see [LICENSE](LICENSE). It is
distributed WITHOUT ANY WARRANTY; see the license for details. [NOTICE](NOTICE) lists the upstream commit and
what Heatbit changed.

"Heatbit", "Bitair" and the Heatbit logo are trademarks of Heatbit Inc. and are not covered by the license.
Bitaxe is a trademark of bitaxeorg; this is not an official Bitaxe product and is not endorsed by bitaxeorg.
