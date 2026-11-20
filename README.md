# Solar Node

Zephyr 4.4 ESP32 firmware for battery telemetry, Wi-Fi, MQTT, Home Assistant discovery, DHT22 sensing, and OTA updates.

## Build

Use VS Code tasks:

1. `Zephyr: Build ESP32 ProCPU (standard)` for normal firmware builds.
2. `Zephyr: Build ESP32 ProCPU (pristine)` after configuration or device-tree changes.
3. `Zephyr: Flash ESP32 ProCPU` to flash over USB.
4. `Zephyr: Monitor Serial (COM5 115200)` to view logs.

The project `.venv` must contain `west`, `esptool`, and `smpclient`.

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements-ota.txt
```

## USB Flashing

USB flashing continues to work after MCUboot is installed:

1. Run `Zephyr: Build ESP32 ProCPU (standard)` for normal application-only builds.
2. Run `Zephyr: Flash ESP32 ProCPU` to flash the build over USB.

Use `Zephyr: Build OTA ESP32 ProCPU (pristine)` followed by
`Zephyr: Flash OTA ESP32 ProCPU` when you need to rebuild or repair MCUboot, or
when OTA configuration changes.

## Regular OTA Update

After MCUboot has been installed once:

1. Run `Zephyr: Build OTA ESP32 ProCPU (pristine)`.
2. Run `OTA: Upload, Test, and Reset` and enter the ESP32 IPv4 address.
3. Wait for the device to reboot.
4. Verify Wi-Fi, MQTT, sensors, and switch control.
5. Run `OTA: Confirm Active Image`. Skip this if the new firmware fails; the
   next reboot should return to the previous image.

The OTA server uses UDP port `1337`. Its signed image is `build/solar-node/zephyr/zephyr.signed.bin`.
