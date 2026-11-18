# Flashing the Heatbit Bitair MainBoard V1.22

This procedure describes how to program a complete merged firmware image onto the ESP32-S3-WROOM-1-N16R8 module fitted to the Heatbit Bitair MainBoard V1.22.

> **Important:** a Bitair that has been provisioned stores its device identity (UUID, worker ID and client certificate) in the NVS partition at `0x9000`. Erasing the flash destroys it, and it cannot be recreated. Use the full procedure below only on a blank board. On a provisioned device write only the application: see [doc/installing-your-own-firmware.md](doc/installing-your-own-firmware.md).

## Safety

> **Warning:** USB-C connector J5 carries data only. Its VBUS pins are not connected, so the USB cable cannot power the board. A regulated 15 V DC supply connected to CN1 is required during programming.

- Switch off the external supply before connecting, disconnecting, or rewiring CN1.
- Verify the supply voltage and polarity before applying power. Reversed polarity may damage the board.
- Use the approved keyed CN1 wiring harness. Do not apply power using clips on exposed pads or test points.
- Keep conductive tools and loose hardware away from the energized board. The board contains exposed high-current power circuitry.
- If the fan or cooling assembly is connected, keep fingers, tools, and cables clear of it.
- Do not operate the device continuously without its intended heatsink and airflow.

## Requirements

- Heatbit Bitair MainBoard V1.22
- The supplied merged Bitair firmware image (`.bin`)
- USB Type-C data cable
- Computer with an available USB port
- Regulated 15 V DC power supply and the approved CN1 harness
- Python 3 with a current version of [esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/)

Install or update esptool if necessary:

```sh
python -m pip install --upgrade esptool
```

## Connections and Controls

| Marking | Reference | Function |
| --- | --- | --- |
| `CN1` | CN1 | External 15 V DC power input |
| USB Type-C | J5 | ESP32-S3 native USB data connection |
| `RST` | SW1 | ESP32-S3 reset/enable button |
| `BOOT0` | SW2 | ESP32-S3 GPIO0 boot-mode button |

CN1 power contacts are:

| CN1 pins | Connection |
| --- | --- |
| 1 and 2 | +15 V DC |
| 3 and 4 | GND / negative return |

CN1 pins 5 through 10 carry other system signals and must not be used as power contacts. Do not use X1 or X2 as the positive power input; they are connected to ground.

## Apply Power

1. Switch off or disable the output of the external power supply.
2. Set the supply to regulated 15 V DC. Use an appropriate current limit for the approved production supply; do not use the board's 3 A input-fuse rating as the normal operating-current specification.
3. Verify the CN1 harness wiring and polarity with a multimeter according to the CN1 power-contact table above.
4. Insert the keyed harness fully into CN1.
5. Enable the 15 V supply.
6. Connect USB-C connector J5 to the computer using a USB data cable.

It is normal to have the external 15 V supply and USB connected at the same time. The USB connection supplies no power to this board.

## Enter Download Mode

GPIO0 must be held low when the ESP32-S3 comes out of reset. Follow this sequence exactly:

1. Press and hold `RST`.
2. While continuing to hold `RST`, press and hold `BOOT0`.
3. Release `RST` while continuing to hold `BOOT0`.
4. Wait approximately one second.
5. Release `BOOT0`.

The ESP32-S3 should now be in its ROM download mode. The serial port can appear as:

- macOS: `/dev/cu.usbmodem*`
- Linux: `/dev/ttyACM*`
- Windows: `COMx`

The port name may change between download mode and normal firmware operation. If necessary, compare the available ports before and after performing the button sequence.

## Check that the Board Is Blank

Replace `<PORT>` with the detected serial port and read the NVS partition:

```sh
python -m esptool \
  --chip esp32s3 \
  --port <PORT> \
  --before no-reset \
  read-flash 0x9000 0x6000 nvs.bin
```

If `nvs.bin` contains anything other than `0xFF` bytes, the board has been provisioned: keep `nvs.bin` and do not continue with the full procedure.

Examples of `<PORT>` are `/dev/cu.usbmodem1101`, `/dev/ttyACM0`, and `COM5`.

## Program the Firmware

Replace `<PORT>` and `<MERGED_FIRMWARE.bin>` with the actual port and supplied merged-image filename. The complete merged image must be written at address `0x00000000`:

```sh
python -m esptool \
  --chip esp32s3 \
  --port <PORT> \
  --baud 921600 \
  --before no-reset \
  --after hard-reset \
  write-flash \
  --flash-mode dio \
  --flash-freq 80m \
  --flash-size 16MB \
  0x00000000 <MERGED_FIRMWARE.bin>
```

Do not program a merged image at `0x10000`. Address `0x00000000` is required because the image includes the bootloader, partition table and application. The merged image spans the flash from `0x00000000` and overwrites the NVS partition with blank data, which is why it is only for blank boards.

The write operation verifies the programmed data. Wait for esptool to report that the operation completed successfully before disconnecting or resetting the board.

If an additional verification pass is required, run:

```sh
python -m esptool \
  --chip esp32s3 \
  --port <PORT> \
  --before no-reset \
  verify-flash 0x00000000 <MERGED_FIRMWARE.bin>
```

## Restart the Device

1. Ensure both `RST` and `BOOT0` are released.
2. If the application does not start automatically after programming, briefly press and release `RST` once.
3. Confirm that the device starts without repeatedly resetting.

## Troubleshooting

### No serial port appears

- Confirm that regulated 15 V power is present at CN1. USB alone cannot power the board.
- Confirm that the USB-C cable supports data; some cables provide charging only.
- Disconnect and reconnect the USB cable, then repeat the download-mode button sequence.
- Try another USB port without an unpowered hub.
- On Linux, confirm that the user has permission to access `/dev/ttyACM*`.

### Esptool cannot connect

- Repeat the sequence: hold `RST`, hold `BOOT0`, release `RST`, wait one second, then release `BOOT0`.
- Close any serial monitor or other application using the same port.
- Confirm that the correct port was selected.

### Connection is lost during programming

- Repeat the download-mode sequence and retry at a lower baud rate by changing `921600` to `460800` or `115200`.
- Check the USB cable and external 15 V supply connections.

### Device does not boot after programming

- Ensure that `BOOT0` is no longer pressed, then briefly press `RST`.
- Program the correct merged Bitair ESP32-S3 image again at `0x00000000`.
- Confirm that the image is intended for the Heatbit Bitair MainBoard and a 16 MB ESP32-S3 module.

## Power Down

1. Switch off or disable the external 15 V supply.
2. Disconnect the USB-C cable.
3. Disconnect CN1 only after external power has been removed.
