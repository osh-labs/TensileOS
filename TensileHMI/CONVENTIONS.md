# TensileHMI Engineering Conventions

Applies to all code under `TensileHMI/`. Read `docs/TAB5_HMI_BUILD_PLAN.md` (repo root) for scope.

## Toolchain
- ESP-IDF **v5.5.5**, target **esp32p4** (M5Stack Tab5 Kit). Installed at `~/esp/esp-idf-v5.5.5` (`. ~/esp/esp-idf-v5.5.5/export.sh`).
- Managed components via `idf_component.yml` (component manager). Pinned majors:
  `espressif/m5stack_tab5 ^1.3.2`, `espressif/usb_host_ftdi_vcp ^2.1.1`, `espressif/esp_hosted`, `espressif/esp_wifi_remote`, `espressif/esp_lvgl_port ^2`.
- C11 for C, C++17 only where a dependency requires it.

## Layout
```
TensileHMI/
  components/<name>/            ESP-IDF component
    core/                       pure C, NO ESP-IDF includes; host-testable
    include/<name>.h            public API
    <name>_esp.c                ESP-IDF glue (drivers, tasks, http, fs)
    CMakeLists.txt, idf_component.yml (if it has managed deps)
  test/host/                    plain CMake project, builds every components/*/core/*.c with host gcc
    test_<name>.c               tests, using test/host/tinytest.h
  spikes/phase0/                Phase 0 bench-spike app (ESP-IDF project)
  tools/                        Python helpers (golden values, PDF checks)
```

## Rules
- Pure logic (parsing, formatting, URL building, PDF byte generation, statistics) lives in `core/` and must compile with `gcc -std=c11 -Wall -Wextra -Werror` on Linux with no ESP-IDF headers.
- Public symbols prefixed with the component short name (`tl_` tensile_link, `tu_` tensile_upload, `tr_` tensile_report, `tp_` tab5_power).
- Errors: return `int`/`esp_err_t` codes; no `abort()` outside init paths; log with `ESP_LOGx(TAG, ...)` where `TAG` is the component name.
- No dynamic allocation in hot paths (sample handling); PSRAM allowed for buffers via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
- **Secrets never in the repo or logs.** Wi-Fi password and Nextcloud app password come from `/sdcard/phase0.json` (Phase 0) or encrypted NVS (later). Never `ESP_LOG` them; redact as `***`.
- Nextcloud: `https://nx80213.your-storageshare.de/remote.php/dav/files/tensile-hmi/TensileOS/`. On HTTP 401/403 stop immediately, never retry (brute-force protection throttles the site IP).
- Time: UTC internally; local time via POSIX TZ, default `EST5EDT,M3.2.0,M11.1.0`.
- File format: TensileCompanion CSV v1 (see build plan Section 2.4) must stay byte-compatible.
- Do not commit; the integrator commits.
