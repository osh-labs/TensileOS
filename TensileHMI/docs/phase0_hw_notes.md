# Phase 0 Hardware Notes — M5Stack Tab5 power, Wi-Fi (C6), display variant

Status: desk research, 2026-10-14. Nothing here has been measured on our unit yet.
**UNVERIFIED** marks anything the bench test must confirm.

## Sources (pinned)

| Tag | Source | Revision |
|---|---|---|
| **M5U** | https://github.com/m5stack/M5Unified | `e125e73f28b6b83e621e48df432af426bb2ff935` (2026-10-13) |
| **DEMO** | https://github.com/m5stack/M5Tab5-UserDemo | `b4e356bc491ca070d54004718dad789c07d5fc93` (2026-09-15) |
| **BSP** | `espressif/m5stack_tab5` 1.3.2, https://components.espressif.com/components/espressif/m5stack_tab5/versions/1.3.2 | package as downloaded |
| **HOSTED2** | https://github.com/espressif/esp-hosted-mcu, branch `release/v2.x` (v2.13.0) | `721a58188a76e9b25c8946558a2559e752879938` |
| **HOSTED3** | same repo, `main` (v3.0.9) | `ce11825936864341e8de470311ecf65f4c637fa9` |
| **WR** | `espressif/esp_wifi_remote` 1.6.5 (registry zip) | commit `ad5497293a362bdd529c76fc8a53f5855484a3f5` |
| **ESPH** | https://devices.esphome.io/devices/M5Stack-Tab5/ | fetched 2026-10-14 |

Line references are `file:line` at the revision above. For GitHub: `https://github.com/<repo>/blob/<sha>/<file>#L<n>`.

Note: the task brief mentions `src/utility/Power_Class.cpp`. In current M5Unified it is
`src/utility/Power_Class.inl` (included from `src/M5Unified.cpp`).

---

## 1. Power

### 1.1 IO expanders (PI4IOE5V6408)

Two expanders on the internal I2C bus (SDA GPIO31, SCL GPIO32; BSP `include/bsp/m5stack_tab5.h:74-75`):
0x43 and 0x44. M5U `src/M5Unified.inl:2192` (`new PI4IOE5V6408_Class(0x43 + i)`); DEMO
`platforms/tab5/components/m5stack_tab5/m5stack_tab5.c:223-224`; BSP `include/bsp/m5stack_tab5.h:651-652`
(`..._ADDRESS_LOW` = 0x43 = `bsp_io_expander_init()`, `..._ADDRESS_HIGH` = 0x44 = `bsp_io_expander1_init()`).

**Expander 0x44 (BSP `expander1`): power pins**

| Pin | Name | Dir | Meaning | Sources |
|---|---|---|---|---|
| P0 | WLAN_PWR_EN | out | ESP32-C6 power, high = on | M5U `Power_Class.inl:159-168` (bit map comment), BSP `m5stack_tab5.h:136` (`BSP_WIFI_EN`) |
| P1, P2 | NC | — | — | M5U `Power_Class.inl:159-168` |
| P3 | USB5V_EN | out | USB-A host-port 5 V, high = on | M5U `Power_Class.inl:1163-1167`, BSP `m5stack_tab5.h:106` (`BSP_USB_EN`), DEMO `m5stack_tab5.c:368` |
| P4 | PWROFF_PULSE | out | Pulse train = power off | M5U `Power_Class.inl:1735-1742`, DEMO `m5stack_tab5.c:406-425` |
| P5 | nCHG_QC_EN | out | Quick-charge input enable, **active low** | M5U `Power_Class.inl:2737-2760`, DEMO `m5stack_tab5.c:330-347` |
| P6 | CHG_STAT | in | Charger status. M5U: high = charging | M5U `Power_Class.inl:2987-2990` |
| P7 | CHG_EN | out | IP2326 charge enable, high = enabled | M5U `Power_Class.inl:2640-2642`, DEMO `m5stack_tab5.c:349-366` |

Init values seen in the wild for 0x44:

| | OUT_SET (0x05) | IO_DIR (0x03) | OUT_H_IM (0x07) | PULL_SEL / PULL_EN | Source |
|---|---|---|---|---|---|
| M5U | `0b10000001` (CHG_EN=1, QC enabled, WLAN on, USB5V via pull-up) | `0b10110001` (P3 input!) | `0b00000110` | `0b00001000` / `0b00001000` (P3 pull-up) | `Power_Class.inl:169-173` |
| DEMO | `0b00001001` then `bsp_set_charge_qc_en(true)` + `bsp_set_charge_en(true)` | `0b10111001` | `0b00000110` | `0b10111001` / `0b11111001` (P6 pull-down) | `m5stack_tab5.c:301-327`, `main/hal/hal_esp32.cpp:61-63` |

M5U drives USB5V_EN and EXT5V_EN through the pull-up/pull-down resistor with the pin as an input
(`Power_Class.inl:1150-1153`, `1163-1167`); the BSP and DEMO drive it as a push-pull output. Both work per
their authors; we use the BSP call (`bsp_feature_enable(BSP_FEATURE_USB, …)`, BSP `src/bsp_feature_en.c`).

**Expander 0x43 (BSP `expander`): for reference** (M5U `Power_Class.inl:143-157`; ESPH):
P0 RF path (L = internal antenna, H = external MMCX), P1 SPK_EN, P2 EXT5V_EN, P3 NC, P4 LCD_RST, P5 TP_RST,
P6 CAM_RST, P7 HP_DET (in). EXT5V_EN has no BSP feature; DEMO toggles it via `bsp_set_ext_5v_en()`
(`m5stack_tab5.c:387`). Not needed for the HMI.

**Gotcha (affects tp_init ordering).** `esp_io_expander_new_i2c_pi4ioe5v6408()` resets the chip on
creation (`espressif/esp_io_expander_pi4ioe5v6408` 1.0.1, `esp_io_expander_pi4ioe5v6408.c:111`,
`reset()` at `:245-258`). The BSP creates the 0x44 handle lazily on the first `bsp_io_expander1_init()`
(BSP `src/bsp_io_expander.c`), so the first BSP/tab5_power call that touches 0x44 drops WLAN_PWR_EN,
USB5V_EN and CHG_EN. `tp_init()` must run before esp_hosted and USB host init; it re-drives all three.

### 1.2 INA226 battery monitor

| Item | Value | Source |
|---|---|---|
| I2C address | 0x41 | M5U `src/utility/Power_Class.hpp:260`; DEMO `main/hal/hal_esp32.cpp:77`; ESPH |
| Shunt | 0.005 Ω | M5U `Power_Class.inl:190`; DEMO `hal_esp32.cpp:80`; ESPH |
| Max current (cal input) | M5U 2.0 A; DEMO and ESPH 8.192 A | M5U `Power_Class.inl:191`; DEMO `hal_esp32.cpp:80` |
| CONFIG | AVG 16, VBUSCT 1.1 ms, VSHCT 1.1 ms, mode 7 (shunt+bus continuous) = **0x0527** | M5U `Power_Class.inl:185-192` with `INA226_Class.inl:29-34`; DEMO `hal_esp32.cpp:78-79` |
| CAL formula | `CAL = 0.00512 / (Current_LSB × R)`, `Current_LSB = Imax / 32768` | M5U `INA226_Class.inl:36-41`; TI INA226 datasheet SBOS547 (https://www.ti.com/lit/ds/symlink/ina226.pdf), "Programming the Calibration Register" |
| LSBs | bus 1.25 mV, shunt 2.5 µV, power = 25 × Current_LSB | M5U `INA226_Class.inl:44-66`; TI datasheet |
| ID check | reg 0xFF = 0x2260 | M5U `INA226_Class.inl:17-24` |
| Current sign | Shunt reads **positive on discharge**; M5U negates to get + = charge | M5U `Power_Class.inl:2813-2817` (comment); ESPH ("positive = discharging") |
| What VBUS measures | Battery pack voltage (2S) | M5U `Power_Class.inl:2453-2455` (battery mV = VBUS×1000), `2547-2550` (per-cell = VBUS×500) |

M5U CAL = trunc(16777.2) = 16777 (LSB ≈ 61 µA, saturates at ±2 A). DEMO rounds the LSB up to a 0.1 mA step
(`components/power_monitor_ina226/src/ina226.cpp:65-87`). **tab5_power uses Imax = 4.096 A → LSB 125 µA,
CAL 8192 exactly**, so a quick-charge current cannot saturate the register. `tp_read()` computes current
from the shunt register (`V_shunt / R`), which does not depend on CAL and so survives an INA226 brown-out reset.

### 1.3 State of charge

| Source | Mapping |
|---|---|
| M5U `Power_Class.inl:2547-2568` | per-cell mV = VBUS × 500; `level = (mv − 3300) × 100 / (4150 − 3350)` → linear, 0 % at 3.30 V/cell, 100 % at 4.10 V/cell (the 3300 vs 3350 offset looks like a typo upstream) |
| ESPH | linear, 6.0 V → 0 %, 8.23 V → 100 % |
| DEMO | shows raw INA226 values only (`main/hal/components/hal_power.cpp:18-25`) |

Nobody publishes a measured NP-F550 curve. tab5_power uses a **PROVISIONAL** piecewise-linear generic Li-ion
resting-voltage table (`components/tab5_power/core/tp_core.c`, `s_soc_2s[]`): 8.40 V = 100 %, 7.64 V = 50 %,
7.46 V = 20 %, 7.36 V = 10 %, 7.20 V = 5 %, 6.60 V = 0 %. Under load the terminal voltage sags, so the
estimate reads low (conservative). Optional IR compensation (`pack_rint_ohm`) stays off until Phase 0
measures the pack resistance.

### 1.4 Charging, QC, power-off

- **Charge enable**: P7 high. M5U `setBatteryCharge()` (`Power_Class.inl:2640-2642`).
- **Charge current**: M5U `setChargeCurrent()` maps 0 → CHG_EN=0; 500 mA → CHG_EN=1, nQC=1 (QC off);
  1000 mA → CHG_EN=1, nQC=0 (QC on) (`Power_Class.inl:2737-2760`). Both M5U (OUT_SET bit5 = 0) and DEMO
  (`setChargeQcEnable(true)`) enable QC by default. The actual charge currents are **UNVERIFIED**.
- **Charging flag**: M5U reads P6 high = charging (`Power_Class.inl:2987-2990`). DEMO and ESPH enable a
  pull-down on P6; M5U enables no pull. **UNVERIFIED** polarity/drive; tab5_power ORs it with
  "battery current > +20 mA".
- **Power-off**: pulse P4. M5U toggles it 10× at 50 ms (`Power_Class.inl:1735-1742`); DEMO does 3× 100 ms
  high/low (`m5stack_tab5.c:406-425`). tab5_power does 5× 100 ms. Whether it works with USB-C attached is
  **UNVERIFIED** (probably not: external power keeps the rail up).
- The BSP does not cover any of this: `BSP_CAPS_BAT 0` (BSP `include/bsp/m5stack_tab5.h:61`).

---

## 2. Wi-Fi via the ESP32-C6 (esp_hosted over SDIO)

### 2.1 What M5Tab5-UserDemo uses

| Item | Value | Source |
|---|---|---|
| IDF | 5.4.2 | DEMO `platforms/tab5/dependencies.lock:214-217` |
| esp_hosted | 1.4.0 | DEMO `platforms/tab5/main/idf_component.yml:5`, `dependencies.lock:80-89` |
| esp_wifi_remote | 0.8.5 | DEMO `main/idf_component.yml:6`, `dependencies.lock:170-189` |
| C6 slave firmware shipped in repo | `ESP32C6-WiFi-SDIO-Interface-V1.4.1-96bea3a_0x0.bin` (flashed at 0x0) | DEMO `platforms/tab5/wifi_c6_fw/`, `flash.sh` |
| Transport | SDIO, 4-bit, 40 MHz, RX streaming mode | DEMO `platforms/tab5/sdkconfig:2872-2898` |
| Pins | CLK 12, CMD 13, D0 11, D1 10, D2 9, D3 8, reset GPIO 15, reset active-low | DEMO `sdkconfig:2881-2903`; same pins in M5U `src/M5Unified.inl:2591` (`hostedSetPins(12,13,11,10,9,8,15)`) and ESPH (slot 1) |
| Slave target | esp32c6 | DEMO `sdkconfig:2875`, `2962` |

**Which slave firmware ships on a new Tab5: UNVERIFIED.** M5Stack's restore guide
(https://docs.m5stack.com/en/guide/tab5/c6_wifi_restore) says the C6 comes with "Wi-Fi SDIO firmware" and
names no version. The only version on record is V1.4.1 in DEMO. Phase 0 must log the slave version at boot
(esp_hosted prints it; also `esp_hosted_get_coprocessor_fwversion()`, HOSTED2 `host/api/src/esp_hosted_api.c:651`).

### 2.2 Host/slave compatibility

- HOSTED2 `docs/troubleshooting.md:88-92`: use the **same** Hosted version on host and slave; mismatches may be incompatible.
- HOSTED3 `docs/migration.md:55-62`: a 3.x host is back-compatible with 0.0.6+/1.x/2.x slaves; a <3.x host with a 3.x slave is unsupported.
- HOSTED2 `Kconfig:133` adds a **`ESP32P4_TAB5_C6_BOARD`** preset (pins above, reset GPIO 15, reset active-low with
  the comment "with the active-high default the slave never leaves reset", `Kconfig:668-683`). First released in
  **esp_hosted 2.12.12** (checked: 2.12.11 registry zip has no TAB5 symbol, 2.12.12 and 2.12.13 do; commit
  `f896115` 2026-07-15). HOSTED3 has the same preset (`host/Kconfig.ext:140`).
- ESPH sets `active_high: true` for the reset line, which contradicts HOSTED2/DEMO. Semantics may differ between
  the two projects. **UNVERIFIED**; follow HOSTED2 + DEMO (active-low).
- WR 1.6.5 depends on `espressif/esp_hosted >=2.11` with no upper bound (`idf_component.yml`); WR 1.6.3 capped it
  at <3.0 and 1.6.4 reverted the cap (WR `CHANGELOG.md`). **Pin esp_hosted explicitly**, otherwise the resolver
  picks 3.x.
- IDF 5.5.5 has no in-tree `components/esp_wifi/remote`, so WR is active (WR `CMakeLists.txt:1-4`) and uses its
  `idf_v5.5` sources.
- SD card (SDMMC slot 0) and C6 (slot 1) share the SDMMC host. IDF 5.5.5 `sdmmc_host_init()` returns OK if already
  initialized (`components/esp_driver_sdmmc/src/sdmmc_host.c:496-501`). Unmounting the SD card while Wi-Fi is up
  must not deinit the host: **UNVERIFIED**, test on the bench.

**Recommendation for Phase 0.** Host esp_hosted `~2.12.13` (has the Tab5 preset, maintained branch, a month old)
+ esp_wifi_remote `~1.6.5`. First boot: log the slave version. If it is not 2.12.x, either (a) flash the matching
slave (`idf.py create-project-from-example "espressif/esp_hosted^2.12.13:slave"`, target esp32c6, via the C6
download pads in the restore guide) or (b) use host-driven slave OTA (esp_hosted ≥2.6, HOSTED3 `docs/migration.md`
index item 2). Fallback if the link fails and we cannot reflash: DEMO's proven pair esp_hosted 1.4.0 +
esp_wifi_remote 0.8.5 (built only on IDF 5.4.2 upstream; on 5.5.5 it is **UNVERIFIED**).

### 2.3 idf_component.yml (app `main/`)

```yaml
dependencies:
  idf: ">=5.5"
  espressif/m5stack_tab5: "^1.3.2"
  espressif/esp_hosted: "~2.12.13"      # pin: esp_wifi_remote 1.6.5 would otherwise pull 3.x
  espressif/esp_wifi_remote: "~1.6.5"
```

### 2.4 sdkconfig.defaults fragment (ESP-IDF v5.5.5, esp_hosted 2.12.x, Tab5)

```
# ---- Wi-Fi via on-board ESP32-C6 (esp_wifi_remote + esp_hosted 2.x over SDIO), M5Stack Tab5 ----
CONFIG_ESP_WIFI_REMOTE_ENABLED=y
CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_ENABLED=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y
CONFIG_ESP32P4_TAB5_C6_BOARD=y
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000
CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW=y
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=15
# Explicit pins (the TAB5 preset sets the same defaults; kept so a preset change cannot move them)
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=13
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=12
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=11
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1=10
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1=9
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1=8
CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y
```

Symbols checked against HOSTED2 `Kconfig` and WR 1.6.5 `Kconfig`/`Kconfig.rpc.in`.
BUILD-CHECK: see section 4.

Power ordering for Wi-Fi: `tp_init()` (C6 power via WLAN_PWR_EN) → `esp_wifi_init()` (esp_hosted resets the
C6 through GPIO15 and enumerates SDIO).

---

## 3. Display variant detection

BSP 1.3.2 detects the panel by probing the touch controller, but the function is `static`
(`src/bsp_display.c:47-52` enum, `:140-194` `bsp_get_board_version()`), so it is **not public API**:

1. `bsp_feature_enable(BSP_FEATURE_TOUCH, true)`, wait 500 ms.
2. If `i2c_master_probe(bus, 0x55)` acks (ST7123 touch, `ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS`,
   `espressif/esp_lcd_touch_st7123` 1.0.2 `include/esp_lcd_touch_st7123.h:31`): read register 0x0000 →
   FW 3 = ST7123 panel, FW 1 = ST7121 panel.
3. Else if 0x14 acks (`ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP`, `espressif/esp_lcd_touch_gt911` 1.2.1
   `include/esp_lcd_touch_gt911.h:43`): ILI9881C + GT911.
4. Else the BSP asserts.

It logs `Discovered board version 1 (LCD ILI9881C, Touch GT911)` / `version 2 (LCD ST7123 …)` /
`version 3 (LCD ST7121 …)` at boot. To show the variant in Diagnostics, repeat steps 2–3 with
`i2c_master_probe()` after `bsp_display_start()` (touch is already powered by then). Note a third
variant exists (ST7121), and M5U separates "Tab5" and "Tab5X" by ESP32-P4 chip revision ≥ 3.0
(`src/M5Unified.inl:2105-2111`); Tab5X adds an EXT power pin on 0x43 P3 (`Power_Class.inl:177-183`).

---

## 4. Bench checklist (Phase 0)

| # | Check | Expect |
|---|---|---|
| 1 | `i2cdetect`-style scan after `tp_init()` | 0x41 (INA226), 0x43, 0x44, 0x32 (RTC) |
| 2 | INA226 IDs | 0xFE = 0x5449, 0xFF = 0x2260 |
| 3 | Bus voltage vs multimeter at pack terminals | within 1 % |
| 4 | Current sign: unplug USB-C | `current_a` < 0; plug in → > 0 |
| 5 | CHG_STAT level while charging / full / unplugged | record; fix polarity in `tp_read()` if needed |
| 6 | QC on/off with a QC charger | record charge current both ways |
| 7 | USB-A 5 V toggle (`tp_usb_a_5v_cycle`) with OpenScale attached | scale re-enumerates |
| 8 | `tp_power_off()` on battery only, then with USB-C | off / record behaviour |
| 9 | Full discharge log (V, I, every 10 s, ~50 % brightness, Wi-Fi on, scale attached) | replace `s_soc_2s[]` and set warn/block/shutdown from the measured curve; measure `pack_rint_ohm` from a load step |
| 10 | esp_hosted slave version log line at boot | decide reflash/OTA (2.2) |
| 11 | SD mount + Wi-Fi at the same time, then SD unmount/remount | Wi-Fi survives |
