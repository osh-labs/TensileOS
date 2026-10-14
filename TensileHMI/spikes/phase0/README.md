# Phase 0 Bench Spike — Tab5 Kit

Throwaway firmware that brings up each risky subsystem on its own and records the results to the SD card. See `docs/TAB5_HMI_BUILD_PLAN.md` Section 9.1 (Phase 0) for the go/no-go criteria.

## What you need

| Item | Notes |
|---|---|
| M5Stack Tab5 Kit (K145) | **NP-F550 battery installed** for every test |
| SparkFun OpenScale with load cell, firmware v1.6 (`src/main.cpp`) | Unmodified firmware |
| USB-A (male) to Mini-B cable | OpenScale to the Tab5 USB-A port |
| USB-C cable | Tab5 to PC for flashing and serial log |
| microSD card (FAT32, ≤32 GB recommended) | High-endurance type preferred |
| ESP-IDF v5.5.5 | `. ~/esp/esp-idf-v5.5.5/export.sh` |

## 1. Prepare the SD card

Create `phase0.json` in the card root. **It contains secrets: keep the card with the device and delete the file when Phase 0 is done.**

```json
{
  "wifi_ssid": "YOUR_SSID",
  "wifi_pass": "YOUR_WIFI_PASSWORD",
  "dav_base_url": "https://nx80213.your-storageshare.de/remote.php/dav/files/tensile-hmi/TensileOS/",
  "dav_user": "tensile-hmi",
  "dav_app_password": "APP-PASSWORD-FOR-tensile-hmi",
  "tz": "EST5EDT,M3.2.0,M11.1.0"
}
```

## 2. Build and flash

```bash
cd TensileHMI/spikes/phase0
idf.py set-target esp32p4     # first time only
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # Windows: -p COMx
```

The ESP32-C6 must be running a compatible esp_hosted slave firmware. See `TensileHMI/docs/phase0_hw_notes.md` if the Wi-Fi test reports an init failure.

## 3. Run the tests (in this order)

| # | Button | What to do / observe | Pass criterion |
|---|---|---|---|
| 1 | Display + touch | Watch the color fills, then tap the four red corner targets | 1280×720 and 4/4 targets. Note the chart redraw time and the I2C scan. |
| 2 | microSD write/read | — | Verify ok; note MB/s |
| 3 | Power + USB-A 5V | Run once on battery only, then again with USB-C connected | Battery voltage plausible (≈7.0–8.4 V); charging flag changes with USB-C |
| 4 | Scale on USB-A | Plug the OpenScale in **after** pressing the button | FTDI 0403:6015 opens; lines received; reaches READY in JSON mode |
| 5 | DTR/RTS reset probe | Scale attached | Records whether DTR or RTS resets the 328P, and the timing |
| 6 | Wi-Fi (ESP32-C6) | — | Connects; note RSSI at the machine location |
| 7 | SNTP + time zone | — | Synced; July/December rule check shows 08/07 |
| 8 | Nextcloud WebDAV | — | PROPFIND 207, MKCOL, PUT 201 of a test file into `TensileOS/Phase0/` |
| 9 | PDF proof page | — | `/sdcard/phase0/phase0_proof.pdf` written; uploaded if WebDAV passed |
| 10 | Battery logger | Toggle on; unplug USB-C; leave running until the low-battery warning; toggle off | `/sdcard/phase0/battery.csv` gives runtime and the discharge curve |
| 11 | Upload report | — | `report.txt` and `battery.csv` appear in Nextcloud `TensileOS/Phase0/` |

Also test once: **unplug the OpenScale during step 4** and confirm a clean "scale detached" message, with no crash or reboot.

## 4. Send back

- `TensileOS/Phase0/` in Nextcloud (or copy `/sdcard/phase0/` from the card)
- The display variant printed on the label on the back of the Tab5
- Anything odd you saw on screen (tearing, touch offset, brownout resets)

## Security notes

- The app password is used only for HTTP Basic auth over TLS and is never logged (shown as `***`).
- On a `401`/`403` the WebDAV test stops immediately and does not retry, to avoid Nextcloud brute-force throttling of the shop's IP.
