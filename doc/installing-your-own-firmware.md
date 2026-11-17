# Installing your own firmware

You can build this firmware, change it and install it on a Bitair you own. This page describes how.

You need physical access to the main board: open the case, power the board through CN1 and connect a USB-C data
cable to J5. [FLASHING_BITAIR_MAINBOARD.md](../FLASHING_BITAIR_MAINBOARD.md) describes the power supply, the
connectors and how to enter download mode.

Read the whole page before you start. Some steps write eFuses, and eFuses cannot be changed back.

## Before you start

- **Keep NVS.** The NVS partition (`0x9000`) holds the device UUID, worker ID and client certificate written at
  manufacturing. They cannot be recreated. Never run `idf.py erase-flash`, never erase `0x9000`, and never write
  the merged image (`bitair-miner_full.bin`) to a provisioned device: it also covers the NVS partition. Write
  only the application, as shown below.
- **Cloud and updates.** Builds without `sdkconfig.defaults.heatbit` do not connect to the Heatbit cloud and do
  not check for Heatbit firmware updates; the device mines with the pool settings stored on it.
- **Tools.** ESP-IDF v5.5.4 for building (see the [readme](../readme.md#building)); `esptool`, `espefuse` and
  `espsecure` come with it.

Partitions used below:

| Partition | Offset | Size |
|---|---|---|
| `nvs` | `0x9000` | `0x6000` |
| `factory` | `0x10000` | 4 MB |
| `otadata` | `0xf10000` | `0x2000` |

The application goes to the `factory` partition. Erasing `otadata` makes the bootloader start it even if the
device has installed an update into an OTA slot before.

## 1. Check the security state of the chip

Put the board in download mode and run:

```sh
espefuse --port <PORT> summary
```

Look at two values:

- `SECURE_BOOT_EN`: whether the bootloader only starts signed applications;
- `SPI_BOOT_CRYPT_CNT`: an odd value (`1`, `7`) means Flash Encryption is enabled.

If `SECURE_BOOT_EN` is `False`, go to step 2. If it is `True`, go to step 3.

## 2. Without Secure Boot

Build with the default configuration and write the application:

```sh
idf.py build
idf.py -p <PORT> app-flash
python -m esptool --chip esp32s3 --port <PORT> erase-region 0xf10000 0x2000
```

Do not add `sdkconfig.defaults.security` to this build. With it, the firmware encrypts NVS with a new eFuse key
on first boot, and the device identity stored in unencrypted NVS can no longer be read.

## 3. With Secure Boot

The bootloader only starts applications signed with a key whose digest is stored in the chip. One of the three
digest slots, `BLOCK_KEY2` (`SECURE_BOOT_DIGEST2`), is left free for the owner of the device.

1. Generate your signing key and keep it safe. If you lose it, you cannot sign images for this device any more.

   ```sh
   espsecure generate-signing-key --version 2 --scheme rsa3072 my_key.pem
   ```

2. Write the digest of your key into the free slot. **This is permanent.**

   ```sh
   espefuse --port <PORT> burn-key-digest BLOCK_KEY2 my_key.pem SECURE_BOOT_DIGEST2
   ```

3. Build with the production configuration, so the application reads the encrypted NVS the same way the Heatbit
   firmware does, and sign the application with your key:

   ```sh
   idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.security" build
   espsecure sign-data --version 2 --keyfile my_key.pem --output app-signed.bin build/bitair-miner.bin
   ```

4. Write the signed application to the `factory` partition and erase `otadata`. If Flash Encryption is enabled
   (step 1), add `--encrypt` to `write-flash`:

   ```sh
   python -m esptool --chip esp32s3 --port <PORT> write-flash --encrypt 0x10000 app-signed.bin
   python -m esptool --chip esp32s3 --port <PORT> erase-region 0xf10000 0x2000
   ```

5. Reset the board. Do not write the bootloader or the partition table: the bootloader already on the device
   checks your signature and starts your application.

## Checking the result

The default configuration turns all logs off. To see them, put the log level into a small extra defaults file and
build into a separate directory:

```sh
echo "CONFIG_LOG_DEFAULT_LEVEL_INFO=y" > sdkconfig.defaults.logs
idf.py -B build-logs -D SDKCONFIG=build-logs/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.logs" build
```

(on a device with Secure Boot, also add `sdkconfig.defaults.security` and sign `build-logs/bitair-miner.bin`).
Install it as above and open the serial console:

```sh
idf.py -B build-logs -p <PORT> monitor
```

If the firmware cannot read the device identity, it logs `Device is not provisioned ... cloud services will not
connect`. Then check that NVS was not erased and that the build uses the same NVS encryption setting as the
device (step 2 or step 3).

## Going back to the Heatbit firmware

Write a Heatbit firmware image the same way: on a device with Secure Boot it must be signed by Heatbit.
