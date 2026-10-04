---
name: solar-node-agent-guide
description: "Guidance for future agents working on the Solar Node Zephyr ESP32 project. Focus on using available VS Code build tasks instead of manual terminal commands."
---

# Solar Node Project Agent Guide

## Project Overview

**Solar Node** is a battery-powered ESP32-based device running Zephyr RTOS that collects solar/battery telemetry and sends it to Home Assistant via MQTT.

**Current Status (2026-10-02):** Core implementation complete and working. WiFi auto-connect, dual-channel ADC voltage reading, and full MQTT telemetry with Home Assistant auto-discovery are all operational. The device is visible in HA with Solar Voltage, Battery Voltage, and RSSI sensors. The codebase is now being reorganized for reliability, new features, and OTA capability.

**Technology Stack:**
- **Firmware:** Zephyr RTOS v4.4.0 (C with C++ entry point)
- **Microcontroller:** ESP32 (Xtensa dual-core)
- **Communication:** WiFi + MQTT (plain, non-TLS)
- **Build System:** CMake + west (Zephyr meta tool)
- **IDE:** VS Code with Zephyr SDK

---

## Build Instructions: USE VS CODE TASKS

**CRITICAL:** This project has pre-configured VS Code tasks. Always use the tasks instead of running `west build` manually in the terminal.

### Available Tasks

In VS Code, use `Ctrl+Shift+B` or **Terminal → Run Task** to access:

1. **Zephyr: Build ESP32 ProCPU (standard)** (default)
   - Incremental build — use this for day-to-day iteration

2. **Zephyr: Build ESP32 ProCPU (pristine)**
   - Full clean rebuild — use after `prj.conf`, `app.overlay`, or toolchain changes

3. **Zephyr: Flash ESP32 ProCPU**
   - Flashes compiled firmware to the device

4. **Zephyr: Monitor Serial (COM5 115200)**
   - Opens serial monitor; press `Ctrl+]` to exit
   - Adjust `COM5` in `tasks.json` if your device port differs

### Build Artifacts

| File | Purpose |
|------|---------|
| `build/zephyr/zephyr.bin` | Raw firmware binary |
| `build/zephyr/zephyr.elf` | ELF with debug symbols |
| `build/zephyr/.config` | Resolved Kconfig |
| `build/zephyr/zephyr.map` | Memory layout / usage |

---

## Project Structure (current)

```
solar-node/
├── src/
│   ├── main.cpp               # Entry point: LED status + WiFi/MQTT startup
│   ├── config.h               # Compile-time credentials (WiFi, MQTT broker)
│   ├── wifi_autoconnect.h/.c  # WiFi connect/poll module with auto-retry
│   ├── mqtt_client.h/.c       # MQTT client: ADC reads, telemetry, HA discovery
├── app.overlay                # Device tree: ADC channels, WS2812 LED strip (I2S)
├── prj.conf                   # Zephyr Kconfig flags
├── CMakeLists.txt             # Build: lists all src/*.c and src/*.cpp
├── west.yml                   # Zephyr v4.4.0 manifest
└── .vscode/
    └── tasks.json             # VS Code build/flash/monitor tasks
```

---

## Hardware Details

- **LED Strip:** WS2812 via I2S0 (GPIO2 data, GPIO32 BCK, GPIO33 WS)
- **Solar voltage input:** ADC1 channel 7 (IO35/J2), divider ratio 12.506×
- **Battery voltage input:** ADC1 channel 6 (IO34/J3), divider ratio 5.865×
- **Battery chemistry:** Lead-Acid / AGM
- **MOSFET output pin:** TBD — likely IO26, IO25, or IO4 (confirm before Phase 2)
- **DHT22 data pin:** TBD — likely IO26, IO25, or IO4 (confirm before Phase 1)

---

## Current Known Issues

| Issue | Cause | Status |
|-------|-------|--------|
| `CONFIG_PM` disabled | Linker conflict with `log_const_soc` symbol | Commented out in `prj.conf` — safe to leave until power optimization work |
| No MQTT reconnect logic | `mqtt_io_worker` does not re-initiate connection after broker drop | Fix in Phase 0 cleanup |
| Duplicate `#include` in `wifi_autoconnect.c` | Copy-paste artifact | Fix in Phase 0 cleanup |
| ADC `channel_setup_dt` called every read cycle | Should be called once at init | Fix in Phase 0 cleanup |
| Credentials hardcoded in `config.h` (plain text) | Prototype shortcut | Migrate to Zephyr Settings in Phase 0 |

---

## Agent Best Practices

1. **Always use VS Code tasks for building** — tasks set up `.venv` and cwd correctly
2. **Pristine build required after:** `prj.conf` changes, `app.overlay` changes, new modules added to `CMakeLists.txt`
3. **Standard build is sufficient for:** changes to `.c` / `.cpp` / `.h` files only
4. **When adding a new module:** create `src/name.h` + `src/name.c`, add `src/name.c` to `CMakeLists.txt`, use `#ifndef` include guards
5. **Zephyr logging:** use `LOG_MODULE_REGISTER(name, LOG_LEVEL_INF)` per file; use `LOG_INF` / `LOG_WRN` / `LOG_ERR`
6. **Code style:** C (not C++) for all new source files; `main.cpp` is the only C++ file and exists because Zephyr requires it for C++ linkage setup
7. **After flash:** run Monitor Serial task to validate logs; look for `=== Solar Node Starting ===` then WiFi and MQTT connection confirmation

---

## Roadmap: Phases 0–4

Each phase is independently buildable and testable. Complete and verify each phase before starting the next. Phases 1 and 2 require GPIO pin confirmation from the hardware owner before device tree work begins.

---

### Phase 0 — Code Cleanup & Refactor
**Goal:** Solid, maintainable foundation before adding features. No new functionality — existing behavior must be identical after this phase.

**Checklist:**
- [ ] Fix duplicate `#include "wifi_autoconnect.h"` and `#include "config.h"` in `src/wifi_autoconnect.c`
- [ ] Remove `solar_mqtt_publish_test()` from `src/mqtt_client.h` and its implementation in `src/mqtt_client.c` (leftover stub, unused)
- [ ] Update stale comment in `src/mqtt_client.h` (still says "stub implementation")
- [ ] Move `adc_channel_setup_dt()` calls from `read_channel_volts()` into `solar_mqtt_init()` — setup once, not every read cycle
- [ ] Merge `publish_sensor_state()` and `publish_discovery_config()` into a single `static void mqtt_publish_str(const char *topic, const char *payload, bool retain)` helper
- [ ] Extract the large inline HA discovery JSON strings from `MQTT_EVT_CONNACK` handler into named `static const char disc_*[]` constants at file scope
- [ ] Add MQTT reconnect: in `mqtt_io_worker`, when `mqtt_connected` goes false, attempt `solar_mqtt_start()` again after a delay
- [ ] **Migrate credentials to Zephyr Settings subsystem:**
  - Enable in `prj.conf`: `CONFIG_SETTINGS=y`, `CONFIG_SETTINGS_SHELL=y`, `CONFIG_NVS=y`, `CONFIG_FLASH=y`, `CONFIG_FLASH_MAP=y`
  - Use `config.h` values as compiled-in defaults for first boot
  - At runtime override via shell: `settings set wifi/ssid "MyNet"` then reboot
  - Modify `wifi_autoconnect.c` and `mqtt_client.c` to load from settings at startup
- [ ] Pristine rebuild; verify HA still shows all three sensors

**Verification:** HA shows Solar Voltage, Battery Voltage, RSSI; credentials can be changed via shell without reflashing

---

### Phase 1 — DHT22 Temperature & Humidity
**Prerequisite:** Confirm DHT22 data GPIO pin (likely IO4, IO25, or IO26) before modifying `app.overlay`.

Zephyr has a built-in `aosong,dht22` sensor driver — no third-party library required.

**Checklist:**
- [ ] Add to `prj.conf`: `CONFIG_DHT=y`
- [ ] Add DHT22 device node to `app.overlay`:
  ```dts
  dht22: dht22 {
      compatible = "aosong,dht22";
      dio-gpios = <&gpio0 PIN GPIO_ACTIVE_HIGH>;
  };
  ```
- [ ] Create `src/dht_sensor.h` and `src/dht_sensor.c`:
  - `dht_sensor_init()` — validates device ready
  - `dht_sensor_read(float *temp_c, float *humidity_pct)` — calls `sensor_sample_fetch()` + `sensor_channel_get(SENSOR_CHAN_AMBIENT_TEMP)` and `SENSOR_CHAN_HUMIDITY`
- [ ] Add `src/dht_sensor.c` to `CMakeLists.txt`
- [ ] Call `dht_sensor_read()` in `mqtt_worker` and pass values into `publish_telemetry()`
- [ ] Add HA Discovery on CONNACK for:
  - Temperature: `device_class: temperature`, `unit_of_measurement: °C`, topic `solar_node/sensor/temperature/state`
  - Humidity: `device_class: humidity`, `unit_of_measurement: %`, topic `solar_node/sensor/humidity/state`

**Note on noise:** DHT22 1-wire can be unreliable near switching regulators or MOSFET drivers. If readings are erratic, consider switching to an SHT31 over I2C (I2C is already enabled in `prj.conf` and `app.overlay`).

**Verification:** HA shows Temperature and Humidity entities under Solar Node device, updating every 60 s

---

### Phase 2 — MOSFET/LED Switch Control via MQTT
**Prerequisite:** Confirm MOSFET gate GPIO pin (likely IO25 or IO26) before modifying `app.overlay`.

MQTT subscribe + GPIO output. HA treats this as a `switch` entity with a `command_topic`. The firmware subscribes, receives ON/OFF payloads, drives the GPIO, and publishes state back.

**Checklist:**
- [ ] Add GPIO output node to `app.overlay` for the MOSFET gate pin
- [ ] Create `src/output_control.h` and `src/output_control.c`:
  - `output_control_init()` — configure pin as GPIO output, default LOW
  - `output_control_set(bool on)` — drive the pin
- [ ] Add `src/output_control.c` to `CMakeLists.txt`
- [ ] In `mqtt_event_handler` on `MQTT_EVT_CONNACK`: subscribe to `solar_node/switch/lights/command`
- [ ] Handle `MQTT_EVT_PUBLISH`: compare payload to `"ON"` / `"OFF"`, call `output_control_set()`, publish new state to `solar_node/switch/lights/state` (retained)
- [ ] Add HA Discovery for switch entity on CONNACK:
  ```json
  {
    "name": "Lights",
    "command_topic": "solar_node/switch/lights/command",
    "state_topic": "solar_node/switch/lights/state",
    "payload_on": "ON", "payload_off": "OFF", "retain": true,
    "unique_id": "solar_node_lights",
    "device": {"identifiers": ["solar_node"], "name": "Solar Node"}
  }
  ```

**Verification:** HA shows a Lights switch; toggling ON/OFF in HA dashboard drives the GPIO (verify with multimeter before connecting MOSFET load)

---

### Phase 3 — Battery State of Charge (Lead-Acid AGM)
**Goal:** Report battery % alongside voltage. No extra hardware required — derived from voltage using an AGM discharge curve.

Note: SoC accuracy is best on resting voltage (no charge/discharge current). Readings may be elevated while the solar panel is charging.

**Checklist:**
- [ ] Create `src/battery_soc.h` and `src/battery_soc.c`:
  - `battery_soc_from_voltage(float volts)` → `int` (0–100)
  - Piecewise linear interpolation over AGM curve:

    | Volts  | SoC % |
    |--------|-------|
    | ≥12.70 | 100   |
    | 12.50  | 90    |
    | 12.40  | 80    |
    | 12.20  | 70    |
    | 12.00  | 50    |
    | 11.90  | 40    |
    | 11.80  | 30    |
    | 11.70  | 20    |
    | 11.60  | 10    |
    | ≤11.60 | 0     |

- [ ] Add `src/battery_soc.c` to `CMakeLists.txt`
- [ ] Call in `mqtt_worker` after reading battery voltage; publish int value to `solar_node/sensor/battery_soc/state`
- [ ] Add HA Discovery on CONNACK: `device_class: battery`, `unit_of_measurement: %`, `suggested_display_precision: 0`

**Verification:** HA shows Battery entity with % value; cross-check at a known voltage against a battery tester

---

### Phase 4 — OTA Firmware Updates (MCUmgr SMP over TCP/IP)
**Goal:** Fully wireless firmware updates from VS Code terminal. The ESP32 serves update requests on port 1337 over WiFi — no external HTTP server needed.

**This phase requires one wired flash to install the updated partition layout and MCUboot. After that, all future updates are wireless.**

**Checklist:**
- [ ] Add to `prj.conf`:
  ```
  CONFIG_BOOTLOADER_MCUBOOT=y
  CONFIG_IMG_MANAGER=y
  CONFIG_MCUMGR=y
  CONFIG_MCUMGR_TRANSPORT_TCP=y
  CONFIG_SMP_SERVER=y
  CONFIG_ZCBOR=y
  CONFIG_MCUMGR_GRP_IMG=y
  CONFIG_MCUMGR_GRP_OS=y
  ```
- [ ] Create a dual-bank partition map (primary + secondary app slots). Check Zephyr ESP32 MCUboot samples for the correct `boards/` overlay or `pm_static.yml` format
- [ ] Pristine build + wired flash (first time only, to install MCUboot + new partition layout)
- [ ] Install `mcumgr` CLI: `pip install mcumgr` or via Go: `go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest`
- [ ] Add VS Code task "OTA: Upload Firmware via MCUmgr" to `.vscode/tasks.json`:
  ```
  mcumgr --conntype tcp --connstring "addr=<ESP_IP>,port=1337" image upload build/zephyr/zephyr.signed.bin
  mcumgr --conntype tcp --connstring "addr=<ESP_IP>,port=1337" image test <hash>
  mcumgr --conntype tcp --connstring "addr=<ESP_IP>,port=1337" os reset
  ```
  The ESP IP is shown in serial monitor logs on boot, or via your router's DHCP table.
- [ ] Test rollback: upload firmware with a distinct log string, confirm boot, then verify MCUboot reverts on a bad image

**Verification:** Upload a firmware change wirelessly; device reboots to new firmware; MQTT telemetry resumes in HA; MCUboot reverts to previous slot if new image fails to confirm

---

## Quick Reference

| Need | Action |
|------|--------|
| Build (fast) | `Ctrl+Shift+B` → "Build ESP32 ProCPU (standard)" |
| Build (clean) | `Ctrl+Shift+B` → "Build ESP32 ProCPU (pristine)" |
| Flash | `Ctrl+Shift+B` → "Flash ESP32 ProCPU" |
| Monitor logs | `Ctrl+Shift+B` → "Monitor Serial (COM5 115200)" |
| Change credentials | `settings set wifi/ssid "X"` via shell (Phase 0+); or edit `src/config.h` and rebuild |
| Add source file | Create `src/name.{h,c}`, add `src/name.c` to `CMakeLists.txt`, pristine rebuild |
| Check memory | Review `Memory region` table in build output |
| OTA update | See Phase 4 mcumgr task (after Phase 4 is implemented) |

---

**Last Updated:** 2026-10-02
**Working Features:** WiFi auto-connect, dual ADC voltage reading (solar + battery), MQTT telemetry, HA auto-discovery (Solar Voltage, Battery Voltage, RSSI)
**Next Phase:** Phase 0 — Code Cleanup & Refactor
