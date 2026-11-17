# Guide for coding agents

Bitair firmware: ESP-IDF firmware for the Heatbit Bitair air purifier with a built-in Bitcoin miner (ESP32-S3, BM1370), forked from
[ESP-Miner](https://github.com/bitaxeorg/ESP-Miner). Read [ARCHITECTURE.md](ARCHITECTURE.md) first: it describes
the components, the startup sequence and where each concern lives.

## Build

ESP-IDF v5.5.4 (the version CI uses). Submodules are required (`libsecp256k1`):

```bash
git submodule update --init --recursive
. <esp-idf>/export.sh
idf.py build                 # firmware
idf.py build merge_bins      # also build/bitair-miner_full.bin (bootloader + partition table + app)
```

`sdkconfig.defaults.heatbit` (backend endpoints, not in git) is added automatically when present. Without it the
firmware builds but is not configured for the Heatbit cloud; `sdkconfig.defaults.heatbit.example` is the
template.

Release builds have all logs off (`CONFIG_LOG_DEFAULT_LEVEL_NONE`). For debugging, build with a higher log level
in a separate build directory.

## Flash

`idf.py -p <port> flash` writes the bootloader, partition table, app and otadata. It does not touch NVS.

**Never run `idf.py erase-flash` or erase the NVS partition (`0x9000`) on a real device**: it holds the device
UUID, worker ID and certificate, written at manufacturing: the device's identity on the backend. It cannot be
recreated by the firmware.

## Tests

Unity tests are in `components/<name>/test`. Build and run them on an ESP32-S3:

```bash
idf.py -C test-ci -B build-test -D IDF_TARGET=esp32s3 build
python -m esptool --chip esp32s3 -p <port> write_flash 0x10000 build-test/unit_test_stratum.bin
idf.py -p <port> monitor     # prints "N Tests 0 Failures"
```

Writing only the app at `0x10000` keeps the device's bootloader, partition table and NVS; flash the firmware
again afterwards. Pure logic is easier to test on the host (see `main/thermal/thermal_governor.c`).

## Rules

- Components never include headers from `main/`. When a component needs application data or actions, add a
  small interface (listener, provider, callback) and register it from `main/` (see ARCHITECTURE.md).
- Backend credentials and TLS settings belong in `components/backend_auth` only. Every channel uses mutual TLS
  with the device certificate.
- Backend hosts, paths and other internal values are Kconfig options without defaults, set in
  `sdkconfig.defaults.heatbit`; never hard-code them. Never commit keys or certificates (`certs/` is ignored).
- Only the power management task starts or stops mining. Faults stop it with `STOP_IMMEDIATE`.
- Do not log secrets: pool passwords, credentials, tokens, keys.
- Keep `sdkconfig.defaults` minimal: only settings that differ from defaults, grouped and commented
  (`idf.py save-defconfig` produces the minimal set).
- Changes to upstream files (`main/tasks`, `components/asic`, ...) should stay small so upstream fixes can still
  be merged; put Bitair behaviour in Bitair files where possible.
