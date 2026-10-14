# TensileOS HMI on M5Stack Tab5 — Build Plan

| Item | Value |
|---|---|
| Document | TAB5_HMI_BUILD_PLAN.md |
| Revision | 3 (owner decisions incorporated, Section 0) |
| Scope | Standalone touchscreen HMI (M5Stack Tab5 Kit) that reaches feature parity with TensileCompanion v1.6.0, adds on-device PDF reporting, and auto-uploads to Nextcloud |
| Baseline reviewed | `src/main.cpp` (firmware), `TensileCompanion/` v1.6.0, sample data in `TensileCompanion/Tests/2026-01-20/` |
| Date | 2026-10-14 |
| Status | Approved scope; ready for Phase 0 |

---

## 0. Decision Record

| # | Decision | Owner input | Effect on plan |
|---|---|---|---|
| 1 | Upload target | Hetzner Storage Share (managed Nextcloud), version 33.0.9 | WebDAV backend only in v1. Hetzner allows unlimited users, so a dedicated service account is used (Section 6). Nextcloud 33 also supports the public-share WebDAV endpoint (29+), which is kept as a fallback. |
| 2 | Hardware | Tab5 Kit (K145) with NP-F550 battery, one unit | The battery is a hard requirement: users report brownouts on the USB-A 5V rail without it. Battery monitoring and low-battery interlocks are added (Section 3.1). Fleet provisioning is dropped. |
| 3 | Firmware changes | Approved, but **as separate PRs after the HMI is complete and tested** | HMI v1 must work with the **unchanged v1.6 firmware**. Firmware work moves to Phase 6, after HMI acceptance (Section 9.2). |
| 4 | Wi-Fi | WPA2/WPA3-Personal (standard password) | The enterprise-Wi-Fi risk is closed. Setup is on-screen scan and password entry. |
| 5 | PDF | On-device PDF required | A dedicated PDF component is in v1 scope (Section 7). |
| 6 | Nextcloud instance | `https://nx80213.your-storageshare.de` | Built into the default upload settings (Section 6.2). Checked 2026-10-14: `status.php` reports 33.0.9, maintenance off; unauthenticated WebDAV `PROPFIND` returns `401` as expected. |
| 7 | Time zone | US Eastern by default, configurable in Settings | Time zone section in the Settings pane (Section 4.4) |

---

## 1. Bottom Line

- **The Tab5 Kit can run the scale directly.** The OpenScale's FT231-family USB-UART (VID 0x0403, PID 0x6015) is supported by Espressif's `usb_host_ftdi_vcp` driver, so the scale plugs straight into the Tab5's USB-A port. The battery must stay installed (decision 2).
- **Framework: ESP-IDF 5.4+**, using the `espressif/m5stack_tab5` BSP (v1.3.x), LVGL 9, `usb_host_ftdi_vcp`, and `esp_hosted`/`esp_wifi_remote` for Wi-Fi through the onboard ESP32-C6. Power management (USB-A 5V enable, charger, INA226 battery monitor) is not covered by the BSP; it gets a small component ported from M5Unified (Section 3.1).
- **Upload: Hetzner Storage Share over WebDAV**, using a dedicated `tensile-hmi` account that owns `TensileOS/` and shares it with the team. The desktop app can point `tests_directory` at the Nextcloud-synced folder and read HMI data with no code change.
- **Store-and-forward.** Every test, PDF report and stats CSV is written to microSD first and uploaded from a persistent queue.
- **On-device PDF:** a minimal PDF 1.4 writer (base-14 Helvetica, vector charts) that reproduces the TensileCompanion statistics report layout, with sharper vector charts in place of the desktop's raster chart.
- **The HMI works with the unchanged v1.6 firmware.** The protocol quirks are handled in the HMI's state machine. Firmware improvements follow as two separate PRs after HMI acceptance.
- **Estimated effort: 7–9 developer-weeks for the HMI**, plus about 1.5 weeks for the two firmware PRs (Section 9). These are estimates for one developer experienced with ESP-IDF.

---

## 2. Review of Current Project

### 2.1 System Summary

| Layer | Implementation | Notes |
|---|---|---|
| Sensor | Load cell into HX711 on SparkFun OpenScale (ATmega328P, FT231 USB-UART) | 10 SPS by default (RATE solder jumper; cut it for 80 SPS) |
| Firmware | `src/main.cpp`, PlatformIO `uno` env, `HX711_MP` 10-point calibration | 115200 baud, 2 Hz report, CSV or JSON output |
| Protocol | Single-character commands: `x` (pause / new test), `r` (resume), `j` (toggle JSON/CSV), `c` (stub) | Modal: the device blocks in `handleMenuInput()` while paused |
| Host | TensileCompanion (Python, Tkinter, matplotlib, reportlab) | Live plot, metadata, date-folder CSV storage, browser, stats, PDF |

Desktop connect sequence (`utils/serial_handler.py`): open the port (DTR resets the 328P), wait 3 s, flush, send `j`, wait 0.8 s, send `x`, wait 0.5 s, flush. The device ends up paused in JSON mode.

### 2.2 Firmware and Protocol Observations

| # | Observation | HMI v1 handling (firmware unchanged) | Firmware PR (Phase 6) |
|---|---|---|---|
| F1 | `j` toggles the output format; it does not set it. After reset the mode is always CSV. | Parse the reply `Output format changed to: JSON/CSV` and resend `j` if needed (Section 5.2). | PR-F1: idempotent `J`/`C` commands |
| F2 | On boot the firmware prints 351 calibration test lines, then waits 5 s. | Show "Scale starting…" with a progress indicator; plug-to-ready is about 8 s. | PR-F1: remove the dump or put it behind a command |
| F3 | No identify or status command. | Recognize TensileOS from the menu text. | PR-F1: `?` returns `{"device":"TensileOS","fw":"x.y.z","mode":"json","paused":true,"peak":0.000}` |
| F4 | Pressing `j` in the menu leaves the menu and resumes measuring. | The state machine sends `x` to pause again. | PR-F1: menu commands other than `r`/`x` keep the device paused |
| F5 | Each reading is `get_units(5)`, which averages about 500 ms at 10 SPS (matches the 0.50 s mean interval in the sample CSVs). **The reported peak is a 500 ms moving average, not the instantaneous peak.** On a linear ramp the average lags the true force by (rate × window / 2). The sample files ramp at 0.49–0.83 kN/s over the 2 s before the peak, so the lag is about 0.12–0.21 kN, or 4–6% of the 3.3–3.8 kN peaks. The peak can be under-reported by up to one more report interval (rate × 0.5 s) when the break falls between reports. Force drops 5–12% in the single sample after the peak in four of the five tests (76% in Test 2), so the breaks happen inside one averaging window. | Record values as reported. The PDF footer states the firmware version and sample rate so the method is traceable. | PR-F2: cut the RATE jumper for 80 SPS, track the peak in firmware on every raw sample, and report a smoothed `current` at 5–10 Hz. Validate against a reference before using results for MBS claims. |
| F6 | The `c` calibration command is a stub, and calibration constants are compiled in. | Out of scope | Possible later PR: calibration in EEPROM plus guided calibration from the HMI |

### 2.3 Desktop App Defects Found During Review

| # | Defect | Evidence | Consequence | HMI design |
|---|---|---|---|---|
| D1 | **Stale sample at the start of a new test.** A reading that was already in flight is appended after `DataManager.clear()` but before the device acts on the reset. | `Tests/2026-01-20/Test_2_211047.csv` row 1: `192.821,0.023,3.379`. The timestamp and peak belong to Test 1; the next row is `0.373,…,0.022`. | `_save_test()` uses `max(peaks)`. If a test's real peak is below the previous test's peak, the saved Peak Force is wrong, and the statistics and MBS are wrong with it. | Samples are accepted only after the firmware's `Starting new test...` acknowledgement, and timestamps are checked (Section 5.3). |
| D2 | **Editing metadata deletes the Project field.** `update_test_metadata()` rewrites the header without `# Project:`; `MetadataEditDialog` has no Project field. | `data/test_manager.py` | Silent data loss whenever metadata is edited. | The rewrite keeps every header key, including unknown ones. |
| D3 | **The PDF results table looks up technician and project by test name.** | `ui/statistics_window.py`, `_export_report()` | Tests that share a name (for example "Test 1" on different days) all show the first match's project and technician. | Rows are keyed by file path. |

D1–D3 are tracked as a separate desktop fix and are not part of the HMI PRs.

### 2.4 Canonical File Format (Must Be Preserved)

```
# Test Name: Test 1
# Date: 2026-01-20 21:06:45
# Technician: Chris Lee
# Project: SEG 5mm Dyneema Cord Tack
# Peak Force: 3.379 kN
# Notes: 
#
timestamp_s,current_kN,peak_kN
0.294,0.068,0.068
```

- Path: `Tests/YYYY-MM-DD/<sanitized_name>_HHMMSS.csv`. The sanitizer replaces `<>:"/\|?*` and spaces with `_` and truncates to 100 characters.
- The HMI adds `# Device: TAB5-<last 3 MAC bytes>` and `# Firmware: <string or "1.6 (inferred)">`. `read_test_metadata()` ignores unknown keys, so the desktop reader stays compatible.
- Statistics definitions to replicate exactly (`analysis/statistics.py`, `ui/statistics_window.py`): sample standard deviation (n−1), median, min/max, 3σ range = mean ± 3s, **3σ MBS = mean − 3s**, with tests sorted by `Date`.

---

## 3. Target Hardware — M5Stack Tab5 Kit (K145)

| Item | Specification | Relevance |
|---|---|---|
| SoC | ESP32-P4, dual-core RISC-V at 360 MHz | Enough headroom for LVGL at 720p, TLS and USB host |
| Wireless | ESP32-C6-MINI-1U (Wi-Fi 6, 2.4 GHz) over SDIO | Wi-Fi goes through `esp_hosted`; 2.4 GHz only |
| Memory | 16 MB flash, 32 MB octal PSRAM | Frame buffers and test buffers fit easily |
| Display | 5", 1280×720 IPS, MIPI-DSI; ILI9881C+GT911 (early units) or ST7123 (current units) | BSP 1.3.x supports both; check the label on the back of the unit |
| USB | USB-A host and USB-C 2.0 OTG | OpenScale on USB-A; USB-C for charging, flashing and logs |
| USB-A 5V | Switched rail (USB5V_EN through a PI4IOE IO expander). M5Unified enables it in `M5.begin()`; in ESP-IDF it must be enabled explicitly. | The OpenScale draws about 80–100 mA at 5 V |
| Battery | NP-F550, 7.4 V, 2000 mAh (14.8 Wh), removable; IP2326 charger through USB-C; INA226 power monitor | **Required for stable USB-A power** (decision 2). Charging works only after the firmware has initialized the power path. |
| Storage | microSD (SDMMC) | Primary storage and upload queue. Use an industrial-grade or high-endurance card. |
| RTC | RX8130CE with supercap backup | Keeps time when there is no network; synced by SNTP |

### 3.1 Power Management (`tab5_power` Component)

The BSP marks battery support as unavailable, so the HMI needs its own small component, ported from the M5Unified Tab5 power code (MIT licensed) and the `M5Tab5-UserDemo` HAL:

| Function | Implementation | HMI behavior |
|---|---|---|
| USB-A 5V enable | IO expander output (USB5V_EN) | Enabled at boot, before USB host install. "Power-cycle scale" button in Diagnostics. |
| Charger path init | IO expander and IP2326 control lines, as in M5Unified | Required for USB-C charging (Section 3) |
| Battery voltage and current | INA226 over the internal I2C bus. M5Unified owns this chip in Arduino; here there is no conflict because M5Unified is not linked. | Status-bar battery percentage, a charging indicator, and a low-battery state |
| Low-battery interlocks | Thresholds from measured discharge curve (Phase 0) | **Below 20%:** warning banner. **Below 10%:** New Test disabled. **Below 5%:** the active test is saved from the journal, then a controlled shutdown. |

Assumption: battery runtime with the display at about 50% brightness, Wi-Fi active and the OpenScale attached has not been measured. Phase 0 measures it. A 14.8 Wh pack at an assumed 3–4 W system draw would give about 3.5–5 h. Plan on keeping USB-C power connected at the test stand, with the battery acting as a buffer.

---

## 4. Software Architecture

### 4.1 Stack

| Function | Component | Notes |
|---|---|---|
| Board support | `espressif/m5stack_tab5` (≥1.3.2) | Display, touch, SD. Needs IDF ≥5.4 |
| Power | `tab5_power` (project component, Section 3.1) | USB-A 5V, charger, INA226 |
| GUI | LVGL 9 through `esp_lvgl_port` | `lv_chart`, `lv_table`, `lv_keyboard` |
| USB serial | `espressif/usb_host_ftdi_vcp` (2.1.x) on `usb_host_cdc_acm` (^2.3) | `FTDI_PID_AUTO` probes 0x6001 and 0x6015 |
| Wi-Fi | `esp_wifi_remote` with `esp_hosted` (P4 host, C6 slave) | Standard `esp_wifi` API; WPA2/WPA3-Personal |
| HTTP/TLS | `esp_http_client` with `esp_crt_bundle` | The Storage Share certificate is expected to chain to a public CA in the bundle. Phase 0 confirms the chain from the device; if it does not, the CA is added to the bundle. |
| Time | SNTP and RX8130CE | RTC stores UTC; local time from a POSIX TZ string (Section 4.4) |
| Storage | FATFS on SD (data), NVS (settings), encrypted NVS for the app password | |
| PDF | `tensile_report` (project component, Section 7) | No third-party dependency |
| OTA | `esp_https_ota` | HMI application only |

### 4.2 Task Model

```
 USB host lib task ──► tensile_link task ──(sample queue)──► session model ──► LVGL task (UI)
   (enumeration,        (FTDI open, line                         │
    FTDI events)         parser, protocol                        ├──► store task (SD journal / CSV / PDF)
                         state machine)                          │
 net task (Wi-Fi, SNTP)  power task (INA226)  ◄── esp_event ────►└──► upload task (queue, retry)
```

- Components communicate only over FreeRTOS queues and `esp_event`. LVGL calls happen only in the LVGL task (or under `lvgl_port_lock`).
- `tensile_link` events: `LINK_READY`, `LINK_GONE`, `SAMPLE`, `TEST_ACK`, `RAW_LINE`.

### 4.3 Repository Layout

```
TensileHMI/                    # ESP-IDF project (new)
├── main/                      # app_main, event wiring
├── components/
│   ├── tab5_power/            # IO expander, USB-A 5V, charger, INA226
│   ├── tensile_link/          # USB VCP + protocol state machine + parser
│   ├── tensile_store/         # CSV read/write, sanitizer, journal, index
│   ├── tensile_stats/         # statistics (parity with analysis/statistics.py)
│   ├── tensile_upload/        # queue + WebDAV backend (backend interface kept for later)
│   ├── tensile_report/        # PDF writer, report layout, stats CSV
│   └── tensile_ui/            # LVGL screens
├── test/host/                 # Linux-target unit tests + golden fixtures
├── tools/                     # golden-value generator (Python, uses TensileCompanion modules)
└── sdkconfig.defaults
```

### 4.4 Time and Time Zone

- The RTC and all internal timestamps are kept in **UTC**. Local time is applied only for display, the `# Date:` header, folder names (`YYYY-MM-DD`) and file names (`HHMMSS`), so desktop-compatible files stay in local time as they are today.
- ESP-IDF converts time with POSIX TZ strings (`setenv("TZ", …)` + `tzset()`); there is no time zone database on the device.
- **Settings → Date & Time** pane:

| Control | Behavior |
|---|---|
| Time zone | Dropdown, default **US Eastern** (`EST5EDT,M3.2.0,M11.1.0`). Also offers US Central (`CST6CDT,M3.2.0,M11.1.0`), Mountain (`MST7MDT,M3.2.0,M11.1.0`), Arizona (`MST7`), Pacific (`PST8PDT,M3.2.0,M11.1.0`), Alaska (`AKST9AKDT,M3.2.0,M11.1.0`), Hawaii (`HST10`) and UTC (`UTC0`) |
| Custom | Free-text POSIX TZ string for any other zone, validated before saving |
| Network time | On by default (SNTP: `pool.ntp.org`, server editable). Off allows manual date and time entry |
| Status | Current local time, UTC offset, DST active yes/no, last SNTP sync |

- The selection is stored in NVS and applied immediately. It changes how future tests are named and stamped only; existing files keep the time they were written with.
- If US DST rules change, the preset strings are updated by an OTA release, or the Custom field covers it immediately.

---

## 5. Plug-and-Play Acquisition Design

### 5.1 User Experience Target

1. The operator plugs the OpenScale into the Tab5 USB-A port.
2. The status bar shows "Scale starting…", then "Scale ready" (about 8 s with v1.6 firmware, because of the F2 boot dump).
3. The operator taps **New Test** (or **Repeat Last**), confirms metadata, and the test starts.
4. The operator taps **Stop & Save**. The CSV goes to SD and the upload queue; the status bar shows the upload state.

### 5.2 Connection State Machine (v1.6 Firmware, No Fixed Sleeps)

```
USB_ATTACH (VID 0403 / PID 6015)
  → OPEN  ftdi_vcp_open(), 115200 8N1
  → RESET set DTR (via set_control_line_state) to force a known boot state
  → BOOT  wait for "Measurement Paused" (menu text); timeout 12 s → retry RESET once → FAULT
  → MODE  send 'j'; expect "Output format changed to: JSON"
          if reply says CSV → send 'j' again (device is now running) → expect JSON
  → PAUSE send 'x'; expect "Measurement Paused"
  → READY (paused, JSON)
NEW_TEST: send 'x' while running (→ paused), then 'x' → expect "Starting new test..." → accept samples
USB_DETACH at any point → LINK_GONE; an active test is kept as "interrupted" and the user is offered save or discard
```

- If DTR does not reset the 328P (Phase 0 checks this), the BOOT step falls back to sending `x` and waiting for the menu text. That path works whether the device was running or paused.
- Firmware detection for PR-F1 (`?` command) is added in a follow-up HMI PR paired with that firmware PR, not in v1 (decision 3).

### 5.3 Data Integrity Rules

- Samples are accepted into a session only between `TEST_ACK` and Stop (fixes D1).
- Any sample whose timestamp is greater than or equal to the last pre-reset timestamp is dropped; the first post-ack sample must be under 2 s.
- Peak Force in the file is `max(peak_kN)` over accepted samples only.
- **Crash-safe journal:** during a test, samples are appended to `/sdcard/.active/session.jnl` every 2 s with `fsync`. On boot, a leftover journal prompts the operator to recover or discard. The low-battery interlock (Section 3.1) uses the same path.
- Final writes are atomic: write `*.tmp`, `fsync`, then rename.

---

## 6. Upload — Hetzner Storage Share (Nextcloud 33.0.9) over WebDAV

### 6.1 Account and Folder Setup (One-Time, by the Owner)

1. In Storage Share administration, create the user `tensile-hmi`. Set a quota of 1 GB; current data volume is about 3 KB per test.
2. Log in as `tensile-hmi` and create the folders `TensileOS/Tests/` and `TensileOS/Reports/`.
3. Share `TensileOS/` from `tensile-hmi` to your own account (or an engineering group) with edit rights. **Because the service account owns the folder, the device's WebDAV path does not depend on where shares are mounted for each user.**
4. As `tensile-hmi`, go to Settings → Security → Devices & sessions and create an app password named `Tab5-HMI`. Enter it on the Tab5 (Settings → Upload). It is stored in encrypted NVS and is never displayed again.
5. Optional: point the Nextcloud desktop client on the engineering PC at the shared `TensileOS/Tests/` folder, and set TensileCompanion's `tests_directory` to that local path.

### 6.2 Protocol

- Base URL: `https://nx80213.your-storageshare.de/remote.php/dav/files/tensile-hmi/TensileOS/`. Host, user and root folder are all editable in Settings → Upload; these values are the defaults.
- Authentication: HTTP Basic with `tensile-hmi:<app password>` over TLS 1.2+
- Per test file:
  1. `MKCOL Tests/YYYY-MM-DD/`. A `201` means created, and `405` means it already exists; both are success.
  2. `PUT Tests/YYYY-MM-DD/<file>.csv` with `If-None-Match: *`, so an existing file is never overwritten. A `201` is success; save the returned `ETag`. On `412`, add `_TAB5` to the name and retry.
  3. A metadata edit on the HMI re-uploads with `If-Match: <etag>`. On `412` (someone edited it in Nextcloud), keep both versions and upload the HMI copy as `<name>_hmi-edit.csv`.
- Reports: `Reports/YYYY-MM-DD/Stats_<HHMMSS>.pdf` and `.csv`, using the same create-only rule.
- Connection test (Settings): `PROPFIND Depth: 0` on the base URL. A `207` shows "Connected". `401` shows "App password rejected". `404` shows "Folder TensileOS not found under tensile-hmi".

### 6.3 Queue and Failure Behavior

- The queue is a manifest on SD (`/sdcard/.queue/*.json`, one entry per pending operation). It survives reboots and battery swaps.
- The upload task runs when Wi-Fi is up and the clock has synced, with exponential backoff from 5 s up to 15 min plus jitter.
- **`401`/`403` stops the queue immediately, with no retries**, and raises a persistent "Upload credentials rejected" banner. Nextcloud's brute-force protection throttles by source IP. The shop's public IP is shared with every user and device on the site, so a device retrying bad credentials could slow Nextcloud logins for everyone there.
- `429`/`503` honor `Retry-After`. Other `5xx` responses and timeouts use backoff.
- Status bar: Wi-Fi RSSI, number of pending uploads, time of last success. Diagnostics shows the queue contents and the last HTTP status for each item.

### 6.4 Fallback: Nextcloud File Drop

If a service account is not wanted, an upload-only public share works on Nextcloud 33. The endpoint is `https://nx80213.your-storageshare.de/public.php/dav/files/<share-token>/…` with the header `X-Requested-With: XMLHttpRequest`, and `anonymous:<share-password>` as Basic auth if the share has a password. Limitations: a File Drop share hides existing files from the uploader, so the device cannot check for or replace a file, and metadata edits cannot be pushed. This is not recommended unless the service account is ruled out.

---

## 7. On-Device PDF Reporting (`tensile_report`)

### 7.1 Writer

| Aspect | Design |
|---|---|
| Format | PDF 1.4, single file, cross-reference table, uncompressed content streams (reports are under 100 KB). Deflate compression is optional later. |
| Fonts | Base-14 Helvetica and Helvetica-Bold with WinAnsiEncoding. No font embedding is required by any compliant viewer. Glyph widths come from the Adobe core-14 AFM metrics, compiled in as a 2 × 256-entry table (about 1 KB), and are used for wrapping and centering. |
| Primitives | Text runs, lines, filled and stroked rectangles, polylines, dashed lines, clipping |
| Character handling | UTF-8 input mapped to WinAnsi. Characters that cannot be mapped are replaced with `?` and logged. The on-screen keyboard is limited to Latin-1 to keep that rare. |
| Memory | Objects are streamed to SD as they are written; only the offset table is held in RAM. |
| Charts | Drawn as vectors (axes, ticks, bars, mean and ±3σ lines), so they print sharply at any scale |

### 7.2 Statistics Report Layout (Parity with `ui/statistics_window.py::_export_report`)

| Element | Desktop (reportlab) | HMI |
|---|---|---|
| Page | US Letter, margins 72/72/36/36 pt | Same |
| Header | Title, company name, generated timestamp, "TensileOS Companion v…" | Same, with "TensileOS HMI v…" and the device ID |
| Summary table | Count, mean, median, min/max, s, 3σ range, 3σ MBS | Same values and format (`%.3f kN`) |
| Chart | 6 × 2.6 in raster PNG (matplotlib) | 6 × 2.6 in vector: bars per test, mean line, ±3σ lines, legend |
| Results table | `#`, Test Name, Project, Technician, Peak (kN), Deviation; header repeated on each page; alternating row shading; wrapped text; centered | Same; rows keyed by file path (fixes D3) |
| Pagination | At least 5 result rows per page | Same rule |
| Footer | "Created via TensileOS" | Same, plus page *n* of *N*, HMI firmware version, and scale firmware and sample rate (traceability for F5) |

### 7.3 Additional Outputs

- **Stats CSV**, matching the desktop "Export Report" CSV columns.
- **Single-test chart PDF** (replaces the desktop "Export Chart" PDF/SVG option): force vs. time with the peak line, plus a metadata block. A PNG option (LVGL snapshot) is kept for quick sharing.

### 7.4 Verification

- Host tests generate PDFs from the golden fixtures and run `qpdf --check` in CI.
- Text extraction (`pdftotext`) is compared with the expected values from the Python golden generator.
- Manual check: the PDF opens in Adobe Acrobat, a browser viewer and the Nextcloud web viewer, and prints correctly on Letter paper.

---

## 8. Feature Parity Matrix

| TensileCompanion feature | Tab5 HMI implementation | Phase |
|---|---|---|
| COM port selection, connect, reconnect | Automatic USB attach and detach; "Reset scale" and "Power-cycle scale" buttons | 1 |
| Auto-switch to JSON mode | Protocol state machine (Section 5.2) | 1 |
| Real-time force vs. time plot, peak line | `lv_chart` with a peak series; decimation above 1,000 points | 1 |
| Large peak readout | Large numeric peak and current values beside the chart | 1 |
| New Test with metadata (name, technician, project, notes) | Full-screen form with `lv_keyboard`; recent-technician and recent-project dropdowns; **Repeat Last** fills the same technician and project and increments the name | 1 |
| Save, Discard, Pause, Resume | Same, with touch targets of at least 80 px | 1 |
| Date-folder CSV storage | Same format and paths on SD (Section 2.4) | 1 |
| Raw serial monitor, debug to console | Diagnostics screen (last 1,000 lines); ESP log over USB-C | 1 |
| — (new) | Battery status and low-battery interlocks | 1 |
| — (new) | Wi-Fi setup (scan and keyboard), SNTP and RTC | 2 |
| — (new) | Time zone selection in Settings, default US Eastern (Section 4.4) | 2 |
| — (new) | WebDAV upload, queue, connection test | 2 |
| Test browser (date tree, select all / folder / individual) | Date list, then test list, with checkboxes and a selection count | 3 |
| Preview pane | Detail panel with metadata and a mini-plot | 3 |
| Load historical test | Opens the test read-only on the Live screen | 3 |
| Edit metadata | Same form; keeps every header key (fixes D2); triggers re-upload | 3 |
| Multi-test statistics | `tensile_stats`; results must match Python to ±0.0005 kN on the golden fixtures | 4 |
| Stats bar chart, deviation table | `lv_chart` and `lv_table` | 4 |
| PDF report | `tensile_report` (Section 7) | 4 |
| Stats CSV export | Section 7.3 | 4 |
| Export chart (PNG/PDF/SVG) | Single-test chart PDF and PNG (SVG dropped) | 4 |
| Settings: colors, grid, axis ranges, autoscale | Settings screen; theme presets instead of a free color picker | 5 |
| — (new) | Settings panes: Date & Time, Wi-Fi, Upload, Display, Technicians, Company, About/OTA | 2, 5 |
| Company name, technician list management | Settings screen | 5 |
| — (new) | OTA update of the HMI app | 5 |

---

## 9. Build Phases and PR Plan

Effort estimates assume one developer experienced with ESP-IDF and LVGL, with the Tab5 Kit and the OpenScale on the bench. They are estimates, not commitments.

### 9.1 HMI (Firmware Unchanged at v1.6)

| Phase | PR | Deliverable | Effort | Exit criteria |
|---|---|---|---|---|
| **0 — Bench spike** | Not merged (branch kept for reference) | BSP display and touch; `tab5_power` USB-A 5V and INA226 read; FTDI opened and lines logged; DTR reset behavior characterized; Wi-Fi joined through the C6; HTTPS `PROPFIND` and `PUT` to Storage Share; minimal one-page PDF written to SD | 4–6 days | All items demonstrated on the Kit with the battery installed. Battery runtime measured. Display variant identified. |
| **1 — Acquisition and Live screen** | HMI-1 | `tensile_link`, `tensile_store`, journal, low-battery interlocks, Live screen, metadata form, local save | 2 wk | 20 consecutive tests saved with no stale-sample artifacts; the files open in TensileCompanion with correct metadata; unplug mid-test and a battery pull both lead to a recoverable session |
| **2 — Connectivity and upload** | HMI-2 | Wi-Fi UI, SNTP/RTC, `tensile_upload` (WebDAV), queue, status bar | 1–1.5 wk | Files appear in Storage Share in the correct date folder; 24 h soak with Wi-Fi forced off and on repeatedly loses and duplicates nothing; the `401` stop and `412` paths are tested |
| **3 — Test browser** | HMI-3 | Browser, preview, load, metadata edit with re-upload | 1 wk | Edit keeps all header keys; `If-Match` conflict handled |
| **4 — Statistics and PDF** | HMI-4 | `tensile_stats`, stats screen, `tensile_report`, stats CSV, chart PDF and PNG | 1.5–2 wk | Stats match Python on the golden fixtures; `qpdf --check` passes; the PDF opens and prints in the viewers listed in Section 7.4 |
| **5 — Settings, OTA, hardening** | HMI-5 | Settings, OTA with rollback, NVS encryption, watchdogs, stand mount | 1 wk | Cold boot to ready under 15 s (v1.6 firmware); OTA rollback tested; 8 h continuous run passes |
| **Acceptance** | — | Owner runs a full test series on the machine | 2–3 days | Sign-off before Phase 6 begins |
| **Total HMI** | | | **≈ 7–9 wk** | |

### 9.2 Firmware (After HMI Acceptance, Separate PRs)

| PR | Content | Effort | Exit criteria |
|---|---|---|---|
| **PR-F1 — Protocol** | `?` status and identify; idempotent `J`/`C`; menu commands other than `r`/`x` keep the device paused; calibration dump behind a command; version string | 2–3 days | TensileCompanion still works unchanged; the HMI still works unchanged |
| **HMI-6 (paired with F1)** | HMI detects firmware through `?` and uses the idempotent commands; falls back to the v1.6 path | 1–2 days | Both firmware versions pass the connection test matrix |
| **PR-F2 — Peak capture** | 80 SPS (RATE jumper cut), peak tracked on every raw sample, smoothed `current` at 5–10 Hz, sample rate in `?` | 3–4 days, plus validation | Comparison against a reference shows the expected peak improvement; the method change is documented |

### 9.3 Verification Strategy

- **Host unit tests** (ESP-IDF Linux target) cover the line parser, the protocol state machine with scripted transcripts captured from the v1.6 firmware, the CSV writer and reader round trip, the sanitizer, statistics, and the PDF writer.
- **Golden fixtures:** the five CSVs in `TensileCompanion/Tests/2026-01-20/`. `tools/` runs TensileCompanion's `TestStatistics` to produce expected values.
- **Cross-compatibility:** HMI-written CSVs are loaded through TensileCompanion's `read_test_metadata()` and `read_test_data()` in CI.
- **Hardware-in-loop:** serial replay of captured transcripts through a USB-UART dongle for repeatable connection and new-test timing tests.

---

## 10. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| USB-A brownout | Low with the battery installed | High | Battery always installed (decision 2); USB-C power at the stand; brownout reset reason logged and shown in Diagnostics |
| Power-path init differs between Tab5 board revisions | Med | Med | Port from the current M5Unified source; Phase 0 confirms charging and USB-A 5V on the actual unit |
| The FT231 on the OpenScale enumerates with an unexpected PID | Low | High | Phase 0 check; `ftdi_vcp_open()` accepts an explicit PID |
| DTR does not reset the 328P | Med | Low | Fallback path in Section 5.2 |
| Nextcloud brute-force throttling of the shop IP | Low | Med | No retries on `401`/`403` (Section 6.3) |
| Hetzner maintenance windows or rate limits | Low | Low | The queue holds data; `Retry-After` is honored |
| Non-Latin-1 characters in metadata | Low | Low | Keyboard limited to Latin-1; replacement is logged |
| microSD wear or corruption | Low | Med | High-endurance card; atomic writes; journal; uploads act as an off-device backup |
| Measurement method (F5) challenged in a strength claim | Med | High | PR-F2 with reference validation before using data for published MBS values; firmware and sample rate printed on every report |

---

## 11. Open Items

1. **Service account setup** (Section 6.1) on `nx80213.your-storageshare.de`, done before Phase 0. Do not send the app password through chat or commit it; enter it on the device.
2. **Desktop defects D1–D3**: schedule as a separate TensileCompanion PR (independent of HMI and firmware work).

---

## 12. References

- M5Stack Tab5 documentation: https://docs.m5stack.com/en/core/Tab5
- M5Stack Tab5 power control (USB-A 5V, battery API): https://docs.m5stack.com/en/arduino/m5tab5/power
- M5Stack Tab5 factory firmware (ESP-IDF): https://github.com/m5stack/M5Tab5-UserDemo
- M5Unified (power management reference): https://github.com/m5stack/M5Unified
- ESPHome Tab5 device config (IO expander USB 5V, INA226): https://devices.esphome.io/devices/m5stack-tab5/
- ESP-IDF BSP `espressif/m5stack_tab5`: https://components.espressif.com/components/espressif/m5stack_tab5
- Espressif `usb_host_ftdi_vcp` (FT232 0x6001, FT231 0x6015): https://github.com/espressif/esp-usb/tree/master/host/class/cdc/usb_host_ftdi_vcp
- SparkFun OpenScale repository (FT231, ATmega328P): https://github.com/sparkfun/OpenScale
- SparkFun OpenScale hookup guide (HX711 RATE jumper, power): https://learn.sparkfun.com/tutorials/openscale-applications-and-hookup-guide
- Hetzner Storage Share general information: https://docs.hetzner.com/storage/storage-share/general
- Nextcloud WebDAV basic operations and public share endpoint: https://docs.nextcloud.com/server/stable/developer_manual/client_apis/WebDAV/basic.html
