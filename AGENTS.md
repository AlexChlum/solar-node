---
name: solar-node-agent-guide
description: "Guidance for future agents working on the Solar Node Zephyr ESP32 project. Focus on using available VS Code build tasks instead of manual terminal commands."
---

# Solar Node Project Agent Guide

## Project Overview

**Solar Node** is a battery-powered ESP32-based device running Zephyr RTOS that collects solar/battery telemetry and sends it to Home Assistant via MQTT.

**Current Status (2026-10-06):** Phases 0–2 complete. WiFi auto-connect, dual-channel ADC voltage, MQTT telemetry, HA auto-discovery, DHT22 temperature/humidity, and MOSFET/LED switch control are all operational. The device is visible in HA with Solar Voltage, Battery Voltage, RSSI, Temperature, Humidity, and a Lights switch (IO32/Pwr3/J16). The switch is also controllable from the Zephyr shell via `regulator enable/disable pwr3`.

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
- **Solar voltage input:** ADC1 channel 7 (IO35/J2), R_bottom = 10kΩ, R_top = 100kΩ (theoretical 11.0×, calibrated 12.506× to match multimeter)
- **Battery voltage input:** ADC1 channel 6 (IO34/J3), R_bottom = 10kΩ, R_top = 47kΩ (theoretical 5.7×, calibrated 5.865× to match multimeter)
- **Battery chemistry:** Lead-Acid / AGM
- **MOSFET output pin:** IO32 / Pwr 3 (J16) on the ESPander board — confirmed for Phase 2
- **DHT22 data pin:** IO26 (gpio0, pin 26) — confirmed and implemented in Phase 1
- **I2S BCK/WS (WS2812):** BCK → IO4, WS → IO16 (relocated from IO32/IO33 to free IO32 for the MOSFET)

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
- [x] Add to `prj.conf`: `CONFIG_DHT=y`
- [x] Add DHT22 device node to `app.overlay`:
  ```dts
  dht22: dht22 {
      compatible = "aosong,dht";   /* family compatible; dht22 property selects DHT22 variant */
      dio-gpios = <&gpio0 26 GPIO_ACTIVE_HIGH>;
      dht22;
      status = "okay";
  };
  ```
- [x] Create `src/dht_sensor.h` and `src/dht_sensor.c`:
  - `dht_sensor_init()` — validates device ready
  - `dht_sensor_read(float *temp_c, float *humidity_pct)` — calls `sensor_sample_fetch()` + `sensor_channel_get(SENSOR_CHAN_AMBIENT_TEMP)` and `SENSOR_CHAN_HUMIDITY`
- [x] Add `src/dht_sensor.c` to `CMakeLists.txt`
- [x] Call `dht_sensor_read()` in `mqtt_worker` and pass values into `publish_telemetry()`
- [x] Add HA Discovery on CONNACK for:
  - Temperature: `device_class: temperature`, `unit_of_measurement: °C`, topic `solar_node/sensor/temperature/state`
  - Humidity: `device_class: humidity`, `unit_of_measurement: %`, topic `solar_node/sensor/humidity/state`

**Note on noise:** DHT22 1-wire can be unreliable near switching regulators or MOSFET drivers. If readings are erratic, consider switching to an SHT31 over I2C (I2C is already enabled in `prj.conf` and `app.overlay`).

**Key lesson (captured for future agents):** In Zephyr 4.4.0 the DHT family uses `compatible = "aosong,dht"` with a `dht22;` boolean property. The `dio-gpios` flags **must** be `(GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN)` — `GPIO_ACTIVE_LOW` because the driver asserts "active" to pull the line LOW for the 18 ms start signal; `GPIO_OPEN_DRAIN` because the pin must release to the external pull-up rather than drive HIGH. Using `GPIO_ACTIVE_HIGH` produces a single blip and no sensor response.

**Verification:** ✅ HA shows Temperature and Humidity entities under Solar Node device, updating every 60 s

---

### Phase 2 — MOSFET/LED Switch Control via MQTT
**Prerequisite:** ✅ Confirmed: MOSFET gate = IO32 / GPO_3 / Pwr 3 (J16) on ESPander board.

MQTT subscribe + `regulator-fixed` DT binding. HA treats this as a `switch` entity. The firmware subscribes, receives ON/OFF payloads, drives IO32 via the Zephyr regulator API, and publishes state back. Also controllable from the shell.

**Checklist:**
- [x] Add `pwr3` `regulator-fixed` node to `app.overlay` (IO32 = gpio1 pin 0, `GPIO_ACTIVE_HIGH`)
- [x] Relocate I2S BCK from IO32 → IO4, WS from IO33 → IO16 in `app.overlay` (IO32/IO33 are GPO_3/GPO_2 on ESPander — they cannot double as I2S clocks)
- [x] Add `CONFIG_REGULATOR=y` and `CONFIG_REGULATOR_SHELL=y` to `prj.conf`
- [x] Create `src/output_control.h` and `src/output_control.c` using `regulator_enable()` / `regulator_disable()` with `regulator_is_enabled()` guard
- [x] Add `src/output_control.c` to `CMakeLists.txt`
- [x] In `mqtt_event_handler` on `MQTT_EVT_CONNACK`: subscribe to `solar_node/switch/lights/command`
- [x] Handle `MQTT_EVT_PUBLISH` using `mqtt_read_publish_payload_blocking()` (payload.data is always NULL in Zephyr MQTT receive events — must read from socket)
- [x] Add HA Discovery for switch entity on CONNACK
- [x] Publish retained `OFF` state on connect

**Key lessons (captured for future agents):**
- IO32 is in ESP32 GPIO bank 1 (`gpio1`); its pin index within that bank is **0**, not 32. Using `gpio0` with pin 32 silently does nothing.
- `evt->param.publish.message.payload.data` is **always NULL** on MQTT receive in Zephyr. Use `mqtt_read_publish_payload_blocking()` to read from the socket buffer. Failing to do so causes an immediate null-pointer fault (EXCCAUSE 28).
- `regulator-fixed` is the correct binding for a GPIO-controlled MOSFET power switch. It exposes the output via both the C regulator API and the shell.
- Shell usage: `regulator enable pwr3` / `regulator disable pwr3` / `regulator status`

**Verification:** ✅ HA shows Lights switch under Solar Node device; ON/OFF in HA toggles IO32; shell can also control it independently

---

### Phase 3 — Battery State of Charge (Lead-Acid AGM)
**Goal:** Report battery SoC % and charging status alongside voltage. Voltage acquisition refactored from raw ADC to Zephyr's `voltage-divider` sensor binding.

Note: SoC accuracy is best on resting open-circuit voltage. Readings reflect "Charging" or "Full" when the solar panel is actively charging.

**Checklist:**
- [x] Refactor voltage acquisition to Zephyr `voltage-divider` sensor binding (Option B):
  - Add `vsolar` and `vbatt` nodes (`compatible = "voltage-divider"`) to `app.overlay`
  - Remove `zephyr,user` ADC direct references; divider math now in DT (`output-ohms` / `full-ohms`)
  - Replace `read_channel_volts()` with `read_sensor_voltage()` using sensor API
  - Add `CONFIG_VOLTAGE_DIVIDER=y` to `prj.conf`
- [x] Create `src/battery_soc.h` and `src/battery_soc.c`:
  - `battery_soc_from_voltage(float volts)` → `int` (0–100)
  - Piecewise linear interpolation over AGM discharge curve:

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

- [x] Add `src/battery_soc.c` to `CMakeLists.txt`
- [x] Compute SoC and clamp in `mqtt_worker`:
  - `solar_v > battery_v + 0.10V` OR `battery_v ≥ 12.70V` → SoC = 100 (charging or full)
  - Otherwise → SoC from AGM curve
- [x] Publish `solar_node/sensor/battery_soc/state` (integer %)
- [x] Add HA Discovery on CONNACK: Battery SoC (`device_class: battery`, `unit_of_measurement: %`, `suggested_display_precision: 0`)

**Key lessons (captured for future agents):**
- The `voltage-divider` sensor binding (`compatible = "voltage-divider"`) wraps an ADC channel and exposes `SENSOR_CHAN_VOLTAGE` in Volts via the standard sensor API. Enable with `CONFIG_VOLTAGE_DIVIDER=y` (or auto-enabled by the DT node).
- `output-ohms` / `full-ohms` only need to express the correct ratio; e.g. `output-ohms = <1000>; full-ohms = <12506>;` for a 12.506× divider. The actual resistor values are not required — only the ratio matters.
- Access the device with `DEVICE_DT_GET(DT_NODELABEL(vsolar))`. The label (`vsolar:`) must be on the DT node.
- `sensor_value` for `SENSOR_CHAN_VOLTAGE` is in Volts: `volts = val.val1 + val.val2 / 1e6f`.
- The `voltage-divider` driver calls `adc_channel_setup_dt` on **every** `sensor_sample_fetch`. This is fine for 60-second intervals; remove the manual `adc_channel_setup_dt` call from init.
- Charging detection via `solar_v > battery_v + delta` is a reasonable proxy for active current flow from panel to battery. A current sensor would be required for exact determination.

**Verification:** ✅ HA shows Battery SoC (%) and Battery Status (Charging/Full/Discharging) under Solar Node device, alongside existing Battery Voltage sensor

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

**Last Updated:** 2026-10-07
**Working Features:** WiFi auto-connect, dual voltage reading via DT `voltage-divider` nodes (solar + battery), MQTT telemetry, HA auto-discovery (Solar Voltage, Battery Voltage, Battery SoC, RSSI, Temperature, Humidity, Lights switch), MOSFET switch (IO32/Pwr3) via MQTT + shell regulator
**Next Phase:** Phase 4 — OTA Firmware Updates (MCUmgr SMP over TCP/IP)
