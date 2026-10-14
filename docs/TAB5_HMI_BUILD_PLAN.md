# TensileOS HMI on M5Stack Tab5 — Build Plan

| Item | Value |
|---|---|
| Document | TAB5_HMI_BUILD_PLAN.md |
| Scope | Standalone touchscreen HMI (M5Stack Tab5) that reaches feature parity with TensileCompanion v1.6.0 and adds Wi-Fi auto-upload |
| Baseline reviewed | `src/main.cpp` (firmware), `TensileCompanion/` v1.6.0, sample data in `TensileCompanion/Tests/2026-01-20/` |
| Date | 2026-10-14 |
| Status | Draft for review |

---

## 1. Bottom Line

- **Feasible with stock hardware.** The Tab5 has a USB-A host port, and the OpenScale's FT231-family USB-UART (VID 0x0403, PID 0x6015) is handled by Espressif's `usb_host_ftdi_vcp` driver. No adapter or extra board is needed.
- **Framework: ESP-IDF 5.4+**, using the `espressif/m5stack_tab5` BSP (v1.3.x), LVGL 9 through `esp_lvgl_port`, `usb_host_ftdi_vcp` for USB serial, and `esp_hosted`/`esp_wifi_remote` for Wi-Fi through the onboard ESP32-C6. Arduino (M5Unified) is not recommended because the USB host VCP stack is an ESP-IDF component.
- **Upload endpoint: Nextcloud over WebDAV.** It needs no new infrastructure, the team can see results right away, and the desktop app can point straight at the Nextcloud-synced folder. Build the uploader behind a backend interface so S3 or a generic HTTPS POST can be added later without rework (Section 6).
- **Store-and-forward.** Every test is written to microSD first and uploaded from a persistent queue. Losing Wi-Fi never loses data.
- **File format stays byte-compatible** with TensileCompanion CSVs, so desktop and HMI data mix freely.
- **Estimated effort: 6–8 developer-weeks**, assuming one developer familiar with ESP-IDF (Section 9). Phase 0 is a 3–5 day bench spike that closes out the hardware unknowns before any UI work starts.
- **Two existing data-integrity defects should be fixed** in the desktop app, and the HMI design avoids both (Section 2.3). There is also a measurement-method issue that affects MBS defensibility (Section 2.2).

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

| # | Observation | Impact on HMI | Recommendation |
|---|---|---|---|
| F1 | `j` toggles the output format; it does not set it. After reset the mode is always CSV. | The host must know the current state. If the device is not reset on connect, sending `j` can switch it into CSV. | HMI: parse the reply `Output format changed to: JSON/CSV` and resend `j` if needed (Section 5.2). Firmware: add idempotent `J`/`C` commands. |
| F2 | On boot the firmware prints 351 calibration test lines, then waits 5 s. | Plug-to-ready takes about 8 s. | Remove the dump or put it behind a command. |
| F3 | No identify or status command. | The host cannot confirm it is talking to TensileOS or read the firmware version. | Add `?`, which returns `{"device":"TensileOS","fw":"x.y.z","mode":"json","paused":true,"peak":0.000}`. |
| F4 | Pressing `j` in the menu leaves the menu and resumes measuring. | Side effect the host has to undo by sending `x`. | Make menu commands leave the device paused unless the command is `r` or `x`. |
| F5 | Each reading is `get_units(5)`. At 10 SPS that averages about 500 ms, which matches the 0.50 s mean interval in the sample CSVs. | **The reported peak is a 500 ms moving average, not the instantaneous peak.** On a linear ramp the average lags the true force by (rate × window / 2). The sample files ramp at 0.49–0.83 kN/s over the 2 s before the peak, so the lag is about 0.12–0.21 kN, or 4–6% of the 3.3–3.8 kN peaks. The peak can be under-reported by up to one more report interval (rate × 0.5 s) when the break falls between reports. Force drops 5–12% in the single sample after the peak in four of the five tests (76% in Test 2), so the breaks happen inside one averaging window. | For MBS work: cut the RATE jumper for 80 SPS, track the peak in firmware on every raw sample, and keep reporting a smoothed `current` at 5–10 Hz. This changes the measurement method, so validate it against a reference before adopting it. |
| F6 | The `c` calibration command is a stub, and calibration constants are compiled in. | Recalibrating requires reflashing. | Out of scope for the HMI v1. A later firmware version could store calibration in EEPROM so the HMI can run a guided calibration. |

The HMI works with the current firmware unchanged (F1 and F4 are handled in the host state machine). F2, F3 and F5 are recommended firmware changes and can be developed in parallel (Phase 1b).

### 2.3 Desktop App Defects Found During Review

| # | Defect | Evidence | Consequence |
|---|---|---|---|
| D1 | **Stale sample at the start of a new test.** A reading that was already in flight is appended after `DataManager.clear()` but before the device acts on the reset. | `Tests/2026-01-20/Test_2_211047.csv` row 1: `192.821,0.023,3.379`. The timestamp and peak belong to Test 1; the next row is `0.373,…,0.022`. | `_save_test()` uses `max(peaks)`. If a test's real peak is below the previous test's peak, the saved `Peak Force` is wrong, and the statistics and MBS are wrong with it. |
| D2 | **Editing metadata deletes the Project field.** `TestManager.update_test_metadata()` rewrites the header without a `# Project:` line, and `MetadataEditDialog` has no Project field. | `data/test_manager.py`, `update_test_metadata()` | Silent data loss whenever metadata is edited. |

HMI mitigations: samples are accepted only after the firmware's `Starting new test...` acknowledgement, and any sample whose timestamp is not lower than the last pre-reset timestamp is dropped. The metadata rewrite keeps every header key, including unknown ones.

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
- The HMI adds `# Device: <hmi-id>` and `# Firmware: <fw>` lines. `read_test_metadata()` ignores unknown keys, so this is backward-compatible with the desktop reader. Note that the desktop `update_test_metadata()` would drop them (same root cause as D2).
- Statistics definitions to replicate exactly (`analysis/statistics.py`, `ui/statistics_window.py`): sample standard deviation (n−1), median, min/max, 3σ range = mean ± 3s, **3σ MBS = mean − 3s**, with tests sorted by `Date`.

---

## 3. Target Hardware — M5Stack Tab5

| Item | Specification | Relevance |
|---|---|---|
| SoC | ESP32-P4, dual-core RISC-V at 360 MHz | Enough headroom for LVGL at 720p, TLS and USB host |
| Wireless | ESP32-C6-MINI-1U (Wi-Fi 6, 2.4 GHz) over SDIO | Wi-Fi goes through `esp_hosted`; 2.4 GHz only |
| Memory | 16 MB flash, 32 MB octal PSRAM | Frame buffers and test buffers fit easily |
| Display | 5", 1280×720 IPS, MIPI-DSI; ILI9881C+GT911 (early units) or ST7123 (current units) | BSP 1.3.x supports both; check the label on the back of the unit |
| USB | USB-A host and USB-C 2.0 OTG | The OpenScale plugs into USB-A |
| USB-A 5V | Switched rail (USB5V_EN through an IO expander). M5Unified enables it by default; in ESP-IDF it must be enabled explicitly. | The OpenScale draws about 80–100 mA at 5 V. The current limit on battery is not documented, so measure it in Phase 0. |
| Storage | microSD (SDMMC) | Primary test storage and upload queue |
| RTC | RX8130CE with supercap backup | Keeps time when there is no network; synced by SNTP |
| Power | NP-F550 7.4 V battery (Tab5 Kit only); USB-C input | Confirm which SKU is being procured |

Unit cost: not verified here. Check current M5Stack pricing for the Tab5 (C145) and the Tab5 Kit (K145, with battery).

---

## 4. Software Architecture

### 4.1 Stack

| Function | Component | Notes |
|---|---|---|
| Board support | `espressif/m5stack_tab5` (≥1.3.2) | Display, touch, SD, audio. Needs IDF ≥5.4 |
| GUI | LVGL 9 through `esp_lvgl_port` | `lv_chart` for the live and stats plots, `lv_keyboard` for text entry |
| USB serial | `espressif/usb_host_ftdi_vcp` (2.1.x) on `usb_host_cdc_acm` (^2.3) | `FTDI_PID_AUTO` probes 0x6001 and 0x6015. Add `usb_host_ch34x_vcp` and `usb_host_cp210x_vcp` to support clone boards later. |
| Wi-Fi | `esp_wifi_remote` with `esp_hosted` (P4 host, C6 slave) | Standard `esp_wifi` API on the P4 side |
| HTTP/TLS | `esp_http_client` with `esp_crt_bundle` | Option to load a custom CA PEM from SD for an on-premises server |
| Time | SNTP and RX8130CE | Sets the `Date` header; time zone is configurable |
| Storage | FATFS on SD (data), NVS (settings and secrets) | Use NVS encryption and flash encryption for production units |
| OTA | `esp_https_ota` | HMI application only. The C6 slave firmware updates separately. |

### 4.2 Task Model

```
 USB host lib task ──► tensile_link task ──(sample queue)──► session model ──► LVGL task (UI)
   (enumeration,        (FTDI open, line                         │
    FTDI events)         parser, protocol                        ├──► store task (SD journal / CSV)
                         state machine)                          │
 net task (Wi-Fi, SNTP) ◄────────── esp_event bus ──────────────►└──► upload task (queue, retry)
```

- Components communicate only over FreeRTOS queues and `esp_event`. LVGL calls happen only in the LVGL task (or under the `lvgl_port_lock`).
- `tensile_link` exposes `connect / new_test / pause / resume` and emits `LINK_READY`, `LINK_GONE`, `SAMPLE`, `TEST_ACK`, and `RAW_LINE` (the last one feeds the diagnostics console).

### 4.3 Repository Layout

```
TensileHMI/                    # ESP-IDF project (new)
├── main/                      # app_main, event wiring
├── components/
│   ├── tensile_link/          # USB VCP + protocol state machine + parser
│   ├── tensile_store/         # CSV read/write, sanitizer, journal, index
│   ├── tensile_stats/         # statistics (parity with analysis/statistics.py)
│   ├── tensile_upload/        # queue + backends (webdav, http_post, s3)
│   ├── tensile_report/        # minimal PDF writer, stats CSV export
│   └── tensile_ui/            # LVGL screens
├── test/host/                 # Linux-target unit tests + golden fixtures
└── sdkconfig.defaults
```

---

## 5. Plug-and-Play Acquisition Design

### 5.1 User Experience Target

1. The operator plugs the OpenScale into the Tab5 USB-A port.
2. The status bar shows "Scale connected — ready" within about 2 s of the firmware banner (about 8 s with current firmware, because of F2).
3. The operator taps **New Test**, picks or enters metadata, and the test starts.
4. The operator taps **Stop & Save**. The file goes to SD and the upload queue; the status bar shows the upload state.

### 5.2 Connection State Machine (No Fixed Sleeps)

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

- If firmware v-next adds `?`, `J` and `C`, the state machine detects that from the `?` reply and uses the idempotent commands. Older firmware falls back to the sequence above.
- **Phase 0 must verify:** whether the OpenScale's DTR line auto-resets the 328P (normal for SparkFun Arduino-compatible designs, but not confirmed from the published documentation), and whether the FT231 line-state request produces the reset edge.

### 5.3 Data Integrity Rules

- Samples are accepted into a session only between `TEST_ACK` and Stop (fixes D1).
- Any sample whose timestamp is greater than or equal to the last pre-reset timestamp is dropped; the first post-ack sample must be under 2 s.
- Peak Force in the file is `max(peak_kN)` over accepted samples only.
- **Crash-safe journal:** during a test, samples are appended to `/sdcard/.active/session.jnl` every 2 s with `fsync`. On boot, a leftover journal prompts the operator to recover or discard.
- Final writes are atomic: write `*.tmp`, `fsync`, then rename.

---

## 6. Upload Endpoint — Options Analysis

### 6.1 Comparison

| Criterion | **Nextcloud WebDAV** | AWS S3, SigV4 on device | S3 via presigned URL from a small server | Local HTTPS server (custom API) |
|---|---|---|---|---|
| New infrastructure | None | Bucket and IAM user | Bucket, IAM, and a signer (Lambda or VM) | Server, storage, backups |
| Device complexity | Low: `MKCOL` and `PUT` with Basic auth | Medium: SigV4 canonical request with HMAC-SHA256 (mbedTLS) | Low: GET URL, then PUT | Low: one POST |
| Credential on device | App password for a dedicated service account (revocable per device) | IAM key, scoped to `s3:PutObject` on one prefix | Short-lived URL; device holds only an API token | API token |
| Team access to results | Immediate, in the existing Nextcloud UI and synced desktop folders | Needs a console, a tool, or a sync job | Same as S3 | Needs its own UI |
| Desktop app integration | Point `tests_directory` at the synced folder; no code change | Needs a sync step | Needs a sync step | Needs a client |
| Works off-site | Yes, if Nextcloud is reachable | Yes | Yes | LAN or VPN only |
| Recurring cost | Existing | S3 storage and requests (small at this data volume) | Plus signer hosting | Server upkeep |
| Best fit | Small team already on Nextcloud | Data lake or analytics pipeline | Fleet with strict credential policy | Database or LIMS integration |

Data volume for scale: a 60 s test at 2 Hz is about 120 rows, roughly 3 KB. Even 1,000 tests a year is under 5 MB, so storage cost and bandwidth do not decide this.

### 6.2 Recommendation

**Primary: Nextcloud WebDAV with a dedicated service account.**

- Create a user such as `tensile-hmi` and generate an app password per device (Settings → Security). An app password can be revoked without affecting other devices. Note that Nextcloud app passwords are not limited to one folder, so the account itself should own or have access to nothing except the data folder.
- Share (or place in a Group Folder) `TensileOS/Tests/` with the engineering group.
- Endpoint: `https://<host>/remote.php/dav/files/<user>/TensileOS/Tests/YYYY-MM-DD/<file>.csv`
- Sequence per file:
  1. `MKCOL …/YYYY-MM-DD/`. A `201` means created, and `405` means it already exists; both are success.
  2. `PUT` with `If-None-Match: *`, which guarantees a file is never silently overwritten. A `201` is success. On `412`, add `_<hmi-id>` to the name and retry.
  3. Metadata edits made on the HMI re-upload with `If-Match: <etag>`, so edits made in Nextcloud are not overwritten.

**Alternative for least privilege: a Nextcloud File Drop (upload-only public share).** Endpoint `https://<host>/public.php/dav/files/<share-token>/…` (Nextcloud 29+), with Basic auth `anonymous:<share-password>` if a password is set, and the header `X-Requested-With: XMLHttpRequest`. The device then cannot read or list anything. The trade-off is that HMI metadata edits cannot be pushed as overwrites. This needs a bench check against your Nextcloud version.

**Design for extension.** `tensile_upload` defines one backend interface: `ensure_dir()`, `put(path, bytes, precondition)`, `test_connection()`. Ship WebDAV in v1. Add `http_post` (multipart, for a future LIMS or n8n webhook) and `s3_sigv4` as separate backends selected in Settings. Avoid S3 unless there is a downstream consumer that needs it.

### 6.3 Upload Queue Behavior

- The queue is a manifest on SD (`/sdcard/.queue/*.json`, one entry per pending operation). It survives reboots.
- The upload task runs when Wi-Fi is up and the clock has synced, with exponential backoff from 5 s up to 15 min plus jitter.
- `401` or `403` stops the queue and raises a persistent "Upload credentials rejected" banner. `5xx` and timeouts retry.
- Status bar: Wi-Fi RSSI icon, number of pending uploads, time of last success.
- Settings provides **Test connection**, which runs `PROPFIND Depth:0` against the root folder and reports the HTTP status in plain language.

---

## 7. Feature Parity Matrix

| TensileCompanion feature | Tab5 HMI implementation | Phase |
|---|---|---|
| COM port selection, connect, reconnect | Automatic USB attach and detach; manual "Reset scale" button | 1 |
| Auto-switch to JSON mode | Protocol state machine (Section 5.2) | 1 |
| Real-time force vs. time plot, peak line | `lv_chart` with a peak series; decimation above 1,000 points | 1 |
| Large peak readout | Large numeric peak and current values beside the chart | 1 |
| New Test with metadata (name, technician, project, notes) | Full-screen form with `lv_keyboard`; recent-technician and recent-project dropdowns; **"Repeat last"** fills the same tech and project and increments the name (`Test 5` → `Test 6`) | 1 |
| Save, Discard, Pause, Resume | Same buttons, sized for gloved touch (minimum 80 px) | 1 |
| Date-folder CSV storage | Same format and paths on SD (Section 2.4) | 1 |
| Raw serial monitor, debug to console | Diagnostics screen with a scrolling raw log (last 1,000 lines); USB log over USB-C | 1 |
| Export chart (PNG/PDF/SVG) | LVGL snapshot to PNG on SD and the upload queue | 4 |
| Test browser (date tree, select all / folder / individual) | Date list, then test list, with checkboxes and a selection count | 3 |
| Preview pane | Detail panel: metadata and mini-plot | 3 |
| Load historical test | Opens the test read-only on the Live screen | 3 |
| Edit metadata | Same form; keeps every header key (fixes D2); triggers re-upload | 3 |
| Multi-test statistics (n, mean, median, min/max, s, 3σ range, 3σ MBS) | `tensile_stats`; results must match Python to ±0.0005 kN on the golden fixtures | 4 |
| Stats bar chart with mean and ±3σ lines | `lv_chart` bar series plus line series | 4 |
| Individual deviation table, chronological | `lv_table` | 4 |
| PDF report (company, version, summary, chart, paginated table) | `tensile_report`: minimal PDF 1.4 writer using base-14 Helvetica (no font embedding) and vector bars and lines; saved to SD and uploaded | 4 |
| Stats CSV export | Written next to the PDF | 4 |
| Settings: colors, grid, axis ranges, autoscale | Settings screen; theme presets instead of a free color picker | 5 |
| Tests and export directory | Fixed SD root plus a configurable remote root | 5 |
| Company name, technician list management | Settings screen | 5 |
| — (new) | Wi-Fi setup: scan and on-screen keyboard, **or** `/sdcard/tensile_config.json` for provisioning many units | 2 |
| — (new) | Upload endpoint configuration, test connection, queue status | 2 |
| — (new) | SNTP and RTC time, time zone | 2 |
| — (new) | Crash-safe journal and recovery | 1 |
| — (new) | OTA update of the HMI app | 5 |

---

## 8. Screen Layout (1280 × 720, Landscape)

```
┌──────────────────────────────────────────────────────────────────────────┐
│ TensileOS HMI   ● Scale: ready   Wi-Fi ▂▄▆  ⇪ 0 pending   2026-10-14 09:41│ status bar 48 px
├───────────────────────────────────────────────────┬──────────────────────┤
│                                                   │  PEAK                │
│                                                   │  3.841 kN            │
│              Force (kN) vs Time (s)               │  CURRENT             │
│                   lv_chart                        │  0.904 kN            │
│                   ~900 × 520                       │  Test 6 · SEG 5mm…   │
│                                                   │  Tech: C. Lee        │
├───────────────────────────────────────────────────┴──────────────────────┤
│ [ New Test ] [ Repeat Last ] [ Pause/Resume ] [ Stop & Save ] [ Discard ]│ action bar 96 px
├──────────────────────────────────────────────────────────────────────────┤
│  Live  |  Tests  |  Statistics  |  Settings  |  Diagnostics              │ tab bar 56 px
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 9. Build Phases

Effort estimates assume one developer experienced with ESP-IDF and LVGL, with a Tab5 and an OpenScale on the bench. They are estimates, not commitments.

| Phase | Deliverable | Effort | Exit criteria |
|---|---|---|---|
| **0 — Bench spike (go/no-go)** | Throwaway firmware: BSP display and touch working; USB-A 5V enabled; FTDI opened and lines logged; DTR reset behavior characterized; Wi-Fi joined through the C6; HTTPS `PUT` to Nextcloud | 3–5 days | All six items demonstrated. USB-A current on battery measured with the OpenScale attached. Display variant identified. |
| **1 — Acquisition and Live screen** | `tensile_link`, `tensile_store`, journal, Live screen, metadata form, local save | 1.5–2 wk | 20 consecutive tests saved with no stale-sample artifacts; the files open in TensileCompanion with correct metadata; unplug mid-test leads to a recoverable session |
| **1b — Firmware protocol update (parallel, optional)** | Firmware v1.7: `?`, `J`/`C`, menu side-effect fix, calibration dump gated; optional 80 SPS peak tracking | 2–3 days (+ validation for F5) | Desktop app still works; HMI detects the version and uses the idempotent commands |
| **2 — Connectivity and upload** | Wi-Fi UI and SD provisioning, SNTP/RTC, `tensile_upload` with WebDAV backend, queue, status bar | 1–1.5 wk | Files appear in Nextcloud in the correct date folder; 24 h soak with Wi-Fi forced off and on repeatedly loses and duplicates nothing; `412` collision path tested |
| **3 — Test browser** | Browser, preview, load, metadata edit with re-upload | 1 wk | Edit keeps all header keys; `If-Match` conflict handled |
| **4 — Statistics and reports** | `tensile_stats`, stats screen, PDF writer, stats CSV, chart PNG | 1–1.5 wk | Stats match Python on the five golden fixtures; the PDF opens in Acrobat and in a browser viewer |
| **5 — Settings, OTA, hardening** | Settings screens, OTA, NVS and flash encryption, watchdogs, enclosure or mount | 1 wk | Cold boot to ready under 10 s; OTA rollback tested; 8 h continuous-run test passes |
| **Total** | | **≈ 6–8 wk** | |

### 9.1 Verification Strategy

- **Host unit tests** (ESP-IDF Linux target) cover the line parser, the protocol state machine with scripted transcripts, the CSV writer and reader round trip, the filename sanitizer, and statistics.
- **Golden fixtures:** the five CSVs in `TensileCompanion/Tests/2026-01-20/`. A Python script runs `TestStatistics` on them and writes expected values that the C tests assert against.
- **Cross-compatibility:** every HMI-written file is loaded by TensileCompanion's `read_test_metadata()` and `read_test_data()` in CI.
- **Hardware-in-loop:** use a scripted serial replay (TensileSIM or a USB-UART dongle playing back captured transcripts) for repeatable connection and new-test timing tests.

---

## 10. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| The FT231 on the OpenScale enumerates with a PID other than 0x6015, or with a custom descriptor | Low | High | Phase 0 check; `ftdi_vcp_open()` accepts an explicit PID |
| USB-A 5V cannot supply the OpenScale on battery, or browns out | Low–Med | High | Phase 0 measurement; fallback is a powered hub or a USB-C PD supply |
| DTR does not reset the 328P | Med | Low | The state machine works without a reset (handles the toggle per F1); firmware `?` removes the ambiguity |
| Corporate Wi-Fi uses WPA2-Enterprise or a captive portal | Unknown | Med | Confirm the network type. If needed, check EAP support in `esp_wifi_remote`, or use an IoT SSID |
| Nextcloud reachable only on the LAN or VPN | Unknown | Low | The queue holds data until the network is reachable; alerts fire after a configurable age |
| LVGL chart performance on long tests | Low | Low | Decimate to 1,000 display points; full data is kept in PSRAM |
| Measurement method (F5) challenged in a strength claim | Med | High | Do the firmware peak-tracking change with a reference validation before using HMI data for published MBS values |

---

## 11. Decisions Needed From the Owner

1. **Endpoint:** confirm Nextcloud WebDAV with a service account (recommended) or File Drop. Also, what Nextcloud version is running? (29+ is needed for the File Drop endpoint.)
2. **Hardware SKU:** Tab5 (C145, no battery) or Tab5 Kit (K145, with battery), and how many units and machines.
3. **Firmware changes:** is Phase 1b (protocol update) approved? Is the 80 SPS peak-tracking change (F5) in scope?
4. **Network:** WPA2-Personal or Enterprise? Is Nextcloud reachable from the shop-floor network?
5. **On-device PDF:** required for v1, or acceptable to defer and use the desktop app's PDF from the synced folder?
6. **Desktop defects D1 and D2:** fix now in TensileCompanion (recommended, low effort) so both hosts produce consistent data.

---

## 12. References

- M5Stack Tab5 documentation: https://docs.m5stack.com/en/core/Tab5
- M5Stack Tab5 power control (USB-A 5V): https://docs.m5stack.com/en/arduino/m5tab5/power
- M5Stack Tab5 factory firmware (ESP-IDF): https://github.com/m5stack/M5Tab5-UserDemo
- ESP-IDF BSP `espressif/m5stack_tab5`: https://components.espressif.com/components/espressif/m5stack_tab5
- Espressif `usb_host_ftdi_vcp` (FT232 0x6001, FT231 0x6015): https://github.com/espressif/esp-usb/tree/master/host/class/cdc/usb_host_ftdi_vcp
- SparkFun OpenScale repository (FT231, ATmega328P): https://github.com/sparkfun/OpenScale
- SparkFun OpenScale hookup guide (HX711 RATE jumper, power): https://learn.sparkfun.com/tutorials/openscale-applications-and-hookup-guide
- Nextcloud WebDAV basic operations and public share endpoint: https://docs.nextcloud.com/server/stable/developer_manual/client_apis/WebDAV/basic.html
