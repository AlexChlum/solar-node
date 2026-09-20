---
name: solar-node-agent-guide
description: "Guidance for future agents working on the Solar Node Zephyr ESP32 project. Focus on using available VS Code build tasks instead of manual terminal commands."
---

# Solar Node Project Agent Guide

## Project Overview

**Solar Node** is a battery-powered ESP32-based device running Zephyr RTOS that collects solar/battery telemetry and sends it to Home Assistant via MQTT.

**Current Status:** Phase 1 (WiFi) - Compiles successfully. Phases 2-5 planned (ADC, MQTT, Power Opt, OTA).

**Technology Stack:**
- **Firmware:** Zephyr RTOS v4.4.0 (C++17)
- **Microcontroller:** ESP32 (Xtensa dual-core)
- **Communication:** WiFi + MQTT
- **Build System:** CMake + west (Zephyr meta tool)
- **IDE:** VS Code with Zephyr SDK

---

## 🚀 Build Instructions: USE VS CODE TASKS

**CRITICAL:** This project has pre-configured VS Code tasks. **Always use the tasks instead of running `west build` manually in the terminal.**

### Available Tasks

In VS Code, use `Ctrl+Shift+B` or **Terminal → Run Task** to access:

1. **Zephyr: Build ESP32 ProCPU (standard)** (default)
   - Runs: `.venv\Scripts\python.exe -m west build -b esp32_devkitc/esp32/procpu .`
   - Incremental build (non-pristine), much faster for normal iteration

2. **Zephyr: Build ESP32 ProCPU (pristine)**
   - Runs: `.venv\Scripts\python.exe -m west build -p -b esp32_devkitc/esp32/procpu .`
   - Full clean rebuild
   - Use this after major config/toolchain changes or if build state seems stale

3. **Zephyr: Flash ESP32 ProCPU**
   - Flashes the compiled firmware to your device
   - Requires successful build first

4. **Zephyr: Monitor Serial (COM5 115200)**
   - Opens a serial monitor in VS Code terminal
   - Shows real-time logs from your ESP32
   - Press `Ctrl+]` to exit the monitor
   - **Note:** Adjust `COM5` if your device is on a different port

### Why Use Tasks Instead of Terminal Commands?

- ✅ Proper environment setup (`.venv` activated automatically)
- ✅ Correct working directory
- ✅ VS Code integration with error/warning panels
- ✅ Consistent with project configuration
- ✅ Faster iteration (tasks are already optimized)

### If You Need to Build

```
# DO THIS:
Press Ctrl+Shift+B → Select "Zephyr: Build ESP32 ProCPU (standard)"

# NOT THIS:
.venv\Scripts\python.exe -m west build -b esp32_devkitc/esp32/procpu .
```

### Build Artifacts

- **Firmware:** `build/zephyr/zephyr.bin`
- **ELF:** `build/zephyr/zephyr.elf`
- **Config:** `build/zephyr/.config`
- **Map file:** `build/zephyr/zephyr.map` (memory usage)

---

## 📁 Project Structure

```
solar-node/
├── src/
│   ├── main.cpp               # Entry point (LED demo + WiFi init)
│   ├── config.h               # WiFi SSID, MQTT broker settings
│   ├── wifi_manager.h/.c      # WiFi connection module (Phase 1)
│   ├── battery_sensor.h       # ADC reading stubs (Phase 2)
│   └── mqtt_client.h          # MQTT client stubs (Phase 3)
├── app.overlay                # Device tree configuration (pinouts)
├── prj.conf                   # Zephyr configuration flags
├── CMakeLists.txt             # Build configuration
├── west.yml                   # Zephyr module manifest
└── .vscode/
    └── tasks.json             # VS Code build tasks
```

---

## 🔧 Common Tasks

| Task | Command | Notes |
|------|---------|-------|
| **Build (default)** | `Ctrl+Shift+B` → "Build ESP32 ProCPU (standard)" | Fast incremental build |
| **Build (clean)** | `Ctrl+Shift+B` → "Build ESP32 ProCPU (pristine)" | Full clean rebuild |
| **Flash** | `Ctrl+Shift+B` → "Flash ESP32 ProCPU" | After successful build |
| **Monitor** | `Ctrl+Shift+B` → "Monitor Serial (COM5 115200)" | View live logs; press Ctrl+] to exit |
| **Clean** | `rm -r build/` | Remove build artifacts (then rebuild) |
| **Config Edit** | Edit `prj.conf` | Then rebuild to apply |

---

## 📋 Development Phases

### ✅ Phase 1: WiFi Connectivity
- **Status:** Complete & tested (builds successfully)
- **Files:** `wifi_manager.{h,cpp}`, `config.h`
- **Next:** Update WiFi credentials in `config.h` and test on hardware

### 📝 Phase 2: ADC Battery Voltage (Planned)
- **Status:** Stubs created, not yet implemented
- **Files:** `battery_sensor.{h,cpp}`
- **Kconfig:** ADC flags already in `prj.conf`
- **Task:** Read battery voltage via GPIO ADC channel

### 📝 Phase 3: MQTT Integration (Planned)
- **Status:** Stubs created, not yet implemented
- **Files:** `mqtt_client.{h,cpp}`
- **Task:** Publish battery readings to HA with auto-discovery

### 📝 Phase 4: Power Optimization (Planned)
- **Status:** Disabled in `prj.conf` due to linking issues
- **Task:** WiFi duty-cycle + deep sleep for battery mode
- **Note:** `CONFIG_PM` & `CONFIG_PM_DEVICE` commented out

### 📝 Phase 5: OTA Design (Planned)
- **Status:** MCUboot configured, no implementation yet
- **Task:** Design MCUboot-based HTTP OTA (firmware updates)

---

## 🛠️ Configuration & WiFi Credentials

**Edit [`src/config.h`](src/config.h) to set your WiFi:**

```cpp
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"
#define MQTT_BROKER_HOST "mqtt.local"
#define MQTT_BROKER_PORT 1883
```

Then rebuild using the VS Code task (Ctrl+Shift+B).

---

## ⚠️ Known Issues & Workarounds

### Issue: Build fails with `log_const_soc` undefined reference
**Cause:** Power management (CONFIG_PM) configuration conflict  
**Workaround:** PM configs are **disabled** in `prj.conf` (commented out)  
**Status:** Safe to leave disabled until Phase 4

### Issue: WiFi connect fails at runtime
**Cause:** Invalid credentials, AP unavailable, or DHCP timeout  
**Status:** Real WiFi connect is implemented; failures are runtime/network related  
**Next:** Verify `src/config.h` credentials and check serial logs for connect result/status

### Issue: Binary blobs missing (libphy.a, etc.)
**Cause:** Espressif HAL requires precompiled WiFi libraries  
**Workaround:** Run `west blobs fetch hal_espressif` (already done)

---

## 🧠 Agent Best Practices

When working on this project, follow these practices:

1. **Always use VS Code tasks for building**
   - Don't run `west build` directly in terminal
   - Tasks handle environment setup correctly

2. **Before making changes:**
   - Understand which Phase you're working on
   - Check `prj.conf` for enabled features
   - Review comments in code about pending phases

3. **After code changes:**
   - Use `Ctrl+Shift+B` to rebuild
   - Prefer **Zephyr: Build ESP32 ProCPU (standard)** for normal iteration
   - Use **Zephyr: Build ESP32 ProCPU (pristine)** when Kconfig/build-system changes require a full rebuild
   - Check the build output for errors
   - Review memory usage in build output (FLASH, DRAM allocation)

4. **When adding new files:**
   - Update `CMakeLists.txt` to include new .cpp sources
   - Add `.h` header guards with `#ifndef / #define / #endif`
   - Follow existing code style (C++17, Zephyr logging)

5. **Serial Console Testing:**
   - After flash, run: `Ctrl+Shift+B` → "Monitor Serial (COM5 115200)"
   - Watch for WiFi connection logs in `src/main.cpp`
   - Look for `LOG_INF()` and `LOG_ERR()` messages
   - Press `Ctrl+]` to exit the monitor

6. **Configuration Changes:**
   - Edit `prj.conf` to enable/disable features
   - Changes to `prj.conf` require a full rebuild (the task handles this)
   - See Zephyr docs for CONFIG_* options

---

## 📚 Documentation & References

- **Zephyr Docs:** https://docs.zephyrproject.org/latest/
- **WiFi Networking in Zephyr:** https://docs.zephyrproject.org/latest/connectivity/networking/api/wifi_interfaces.html
- **ESP32 Pinout:** Check `app.overlay` for I2S/GPIO mappings
- **MQTT:** Home Assistant MQTT integration guide

---

## 🤝 For Future Development

When implementing new phases:

1. **Phase 2 (ADC):** Check available GPIO pins in `app.overlay`, configure ADC in `prj.conf`
2. **Phase 3 (MQTT):** Use Zephyr's `CONFIG_MQTT_LIB` (already in prj.conf), implement discovery
3. **Phase 4 (Power):** Re-enable `CONFIG_PM` and `CONFIG_PM_DEVICE`, test deep sleep/wakeup
4. **Phase 5 (OTA):** Implement HTTP firmware download + MCUboot partition handling

All phases are designed to be modular—implement one at a time and test each before moving to the next.

---

## 💡 Quick Reference

| Need | Action |
|------|--------|
| Build (fast) | `Ctrl+Shift+B` → "Build ESP32 ProCPU (standard)" |
| Build (clean) | `Ctrl+Shift+B` → "Build ESP32 ProCPU (pristine)" |
| Flash | `Ctrl+Shift+B` → "Flash ESP32 ProCPU" |
| Monitor Logs | `Ctrl+Shift+B` → "Monitor Serial (COM5 115200)" |
| Edit WiFi | `src/config.h` (edit SSID/password, rebuild) |
| Add Feature | Edit `prj.conf`, implement .cpp/.h, update `CMakeLists.txt` |
| Check Memory | Review `Memory region` section in build output |
| New Module | Create `src/module_name.{h,cpp}`, add to `CMakeLists.txt` |

---

**Last Updated:** 2026-09-19  
**Project Phase:** 1 (WiFi) - Complete & Building  
**Next Target Phase:** 2 (ADC)
