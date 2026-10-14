/*
 * tab5_power: M5Stack Tab5 power path (USB-A 5 V, IP2326 charger control,
 * INA226 battery monitor, power-off) for ESP-IDF v5.5 + espressif/m5stack_tab5.
 *
 * Hardware facts and sources: TensileHMI/docs/phase0_hw_notes.md.
 *
 * Ordering: call tp_init() early in app_main, BEFORE esp_hosted / Wi-Fi init
 * and BEFORE USB host install. The first BSP call that creates the 0x44 IO
 * expander handle resets that chip, which drops WLAN_PWR_EN, USB5V_EN and
 * CHG_EN; tp_init() re-drives them per tp_config_t.
 *
 * Thread safety: tp_* calls are serialized by an internal mutex. BSP calls
 * that touch the same expander (bsp_feature_enable USB/WIFI) from other tasks
 * are not covered by it; route those through tp_* after tp_init().
 */
#ifndef TAB5_POWER_H
#define TAB5_POWER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "tab5_power_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool usb_a_5v_on;        /* drive USB5V_EN (0x44 P3) at init. Default true. */
    bool charge_enable;      /* CHG_EN (0x44 P7). Default true. */
    bool charge_qc_enable;   /* nCHG_QC_EN (0x44 P5, active low). Default true (M5Unified + UserDemo). */
    bool wifi_power_on;      /* WLAN_PWR_EN (0x44 P0) for the C6. Default true. */
    double shunt_ohm;        /* default TP_TAB5_SHUNT_OHM (0.005) */
    double max_current_a;    /* default TP_TAB5_MAX_CURRENT_A (4.096) */
    float pack_rint_ohm;     /* SoC IR compensation; default 0 (off) until Phase 0 measures it */
    tp_lowbat_cfg_t lowbat;  /* default TP_LOWBAT_CFG_DEFAULT (20/10/5 %, 2 % hyst, N=3) */
} tp_config_t;

#define TP_CONFIG_DEFAULT() {                     \
    .usb_a_5v_on = true,                          \
    .charge_enable = true,                        \
    .charge_qc_enable = true,                     \
    .wifi_power_on = true,                        \
    .shunt_ohm = TP_TAB5_SHUNT_OHM,               \
    .max_current_a = TP_TAB5_MAX_CURRENT_A,       \
    .pack_rint_ohm = 0.0f,                        \
    .lowbat = TP_LOWBAT_CFG_DEFAULT,              \
}

typedef struct {
    bool valid;               /* false if the INA226 read failed */
    float bus_v;              /* INA226 VBUS = battery pack voltage (2S) */
    float shunt_mv;           /* raw shunt voltage, mV (+ = discharge on Tab5 wiring) */
    float current_a;          /* battery current, + = charging, - = discharging */
    float power_w;            /* INA226 power register (always >= 0) */
    float soc_pct;            /* 0..100, PROVISIONAL curve */
    bool chg_stat;            /* raw CHG_STAT (0x44 P6) level; M5Unified: high = charging (UNVERIFIED) */
    bool charging;            /* chg_stat || current_a > +20 mA */
    tp_lowbat_state_t lowbat; /* low-battery state after this reading */
} tp_reading_t;

/* Initialize. cfg may be NULL for TP_CONFIG_DEFAULT(). Safe to call once;
 * a second call returns ESP_ERR_INVALID_STATE. Returns ESP_ERR_NOT_FOUND if
 * the INA226 does not answer with the expected IDs at 0x41 (the expander
 * outputs are still configured in that case). */
esp_err_t tp_init(const tp_config_t *cfg);

/* USB-A host-port 5 V rail (wraps bsp_feature_enable(BSP_FEATURE_USB, on)). */
esp_err_t tp_usb_a_5v_set(bool on);

/* Power-cycle the USB-A rail: off, wait off_ms (>= 100 recommended), on. */
esp_err_t tp_usb_a_5v_cycle(uint32_t off_ms);

/* IP2326 charge enable (0x44 P7, high = enabled). */
esp_err_t tp_charge_enable(bool on);

/* Quick-charge input enable (0x44 P5 nCHG_QC_EN, written inverted). */
esp_err_t tp_charge_qc_enable(bool on);

/* Read INA226 + CHG_STAT, compute SoC and advance the low-battery state machine.
 * Call at a fixed period (e.g. 1-5 s) from one task. */
esp_err_t tp_read(tp_reading_t *out);

/* Current low-battery state without a new reading. */
tp_lowbat_state_t tp_lowbat_state(void);

/* Clear a latched SHUTDOWN (e.g. after the user plugged in USB-C and confirmed). */
void tp_lowbat_clear(void);

/* Pulse PWROFF_PULSE (0x44 P4). On battery this cuts power and does not
 * return. Returns ESP_FAIL if the device is still running afterwards
 * (expected when USB-C power is present - UNVERIFIED). The caller must have
 * flushed files and unmounted the SD card first. */
esp_err_t tp_power_off(void);

#ifdef __cplusplus
}
#endif

#endif /* TAB5_POWER_H */
