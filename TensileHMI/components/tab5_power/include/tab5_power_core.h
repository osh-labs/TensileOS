/*
 * tab5_power core: pure C (no ESP-IDF headers), host-testable.
 *
 *  - INA226 register math (config word, calibration, raw -> V / A / W)
 *  - 2S Li-ion state-of-charge estimate from pack voltage (piecewise linear)
 *  - Low-battery state machine with hysteresis and N-consecutive-reading debounce
 *
 * Sources for the constants are listed in TensileHMI/docs/phase0_hw_notes.md.
 */
#ifndef TAB5_POWER_CORE_H
#define TAB5_POWER_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes (core). 0 = OK, negative = error.
 * ------------------------------------------------------------------------- */
#define TP_OK            0
#define TP_ERR_ARG      (-1)
#define TP_ERR_RANGE    (-2)

/* ---------------------------------------------------------------------------
 * INA226 (TI SBOS547). Register map and fixed LSBs.
 * ------------------------------------------------------------------------- */
#define TP_INA226_REG_CONFIG      0x00
#define TP_INA226_REG_SHUNT_V     0x01
#define TP_INA226_REG_BUS_V       0x02
#define TP_INA226_REG_POWER       0x03
#define TP_INA226_REG_CURRENT     0x04
#define TP_INA226_REG_CAL         0x05
#define TP_INA226_REG_MASK_EN     0x06
#define TP_INA226_REG_ALERT_LIM   0x07
#define TP_INA226_REG_MFG_ID      0xFE
#define TP_INA226_REG_DIE_ID      0xFF

#define TP_INA226_MFG_ID          0x5449u /* "TI" */
#define TP_INA226_DIE_ID          0x2260u /* M5Unified INA226_Class::begin() checks this */
#define TP_INA226_CONFIG_RESET    0x8000u /* bit 15 RST */

#define TP_INA226_BUS_LSB_V       0.00125   /* 1.25 mV / bit */
#define TP_INA226_SHUNT_LSB_V     0.0000025 /* 2.5 uV / bit */
#define TP_INA226_CAL_CONST       0.00512   /* CAL = 0.00512 / (Current_LSB * Rshunt) */
#define TP_INA226_POWER_LSB_MULT  25.0      /* Power_LSB = 25 * Current_LSB */

/* AVG field (bits 11..9) */
typedef enum {
    TP_INA226_AVG_1 = 0, TP_INA226_AVG_4, TP_INA226_AVG_16, TP_INA226_AVG_64,
    TP_INA226_AVG_128, TP_INA226_AVG_256, TP_INA226_AVG_512, TP_INA226_AVG_1024
} tp_ina226_avg_t;

/* VBUSCT / VSHCT fields (bits 8..6 / 5..3) */
typedef enum {
    TP_INA226_CT_140US = 0, TP_INA226_CT_204US, TP_INA226_CT_332US, TP_INA226_CT_588US,
    TP_INA226_CT_1100US, TP_INA226_CT_2116US, TP_INA226_CT_4156US, TP_INA226_CT_8244US
} tp_ina226_ct_t;

/* MODE field (bits 2..0) */
typedef enum {
    TP_INA226_MODE_POWER_DOWN = 0,
    TP_INA226_MODE_SHUNT_TRIG = 1,
    TP_INA226_MODE_BUS_TRIG = 2,
    TP_INA226_MODE_SHUNT_BUS_TRIG = 3,
    TP_INA226_MODE_SHUNT_CONT = 5,
    TP_INA226_MODE_BUS_CONT = 6,
    TP_INA226_MODE_SHUNT_BUS_CONT = 7
} tp_ina226_mode_t;

/* Tab5 hardware defaults (see phase0_hw_notes.md, "INA226").
 * Shunt 5 mOhm and addr 0x41 agree in M5Unified and M5Tab5-UserDemo.
 * Max current: M5Unified uses 2.0 A, UserDemo 8.192 A; we use 4.096 A so a
 * QC-charge current cannot saturate the CURRENT register and the numbers are
 * exact (Current_LSB = 125 uA, CAL = 8192). */
#define TP_TAB5_INA226_ADDR       0x41
#define TP_TAB5_SHUNT_OHM         0.005
#define TP_TAB5_MAX_CURRENT_A     4.096

typedef struct {
    uint16_t cal;          /* value for REG_CAL (bit 15 must be 0) */
    double current_lsb_a;  /* effective A/bit after CAL truncation */
    double power_lsb_w;    /* 25 * current_lsb_a */
    double shunt_ohm;
} tp_ina226_cal_t;

/* Build the CONFIG register word (reserved bits 14..12 left 0, as M5Unified). */
uint16_t tp_ina226_config_word(tp_ina226_avg_t avg, tp_ina226_ct_t vbus_ct,
                               tp_ina226_ct_t vsh_ct, tp_ina226_mode_t mode);

/* Compute calibration from shunt resistance and max expected current.
 * Returns TP_ERR_ARG for non-positive inputs, TP_ERR_RANGE if CAL would be 0
 * or exceed 0x7FFF. */
int tp_ina226_calibrate(double shunt_ohm, double max_current_a, tp_ina226_cal_t *out);

/* Raw register -> engineering units. */
double tp_ina226_bus_volts(uint16_t raw);                       /* unsigned, bit15 always 0 */
double tp_ina226_shunt_volts(uint16_t raw);                     /* two's complement */
double tp_ina226_current_amps(uint16_t raw, const tp_ina226_cal_t *cal); /* two's complement */
double tp_ina226_power_watts(uint16_t raw, const tp_ina226_cal_t *cal);  /* unsigned */
/* Current from the shunt register alone (independent of CAL). */
double tp_ina226_current_from_shunt(uint16_t raw_shunt, double shunt_ohm);

/* Tab5 wiring: the shunt reads positive on discharge (M5Unified negates it so
 * that + = charge). tp_* battery current convention: + = charging, - = discharging. */
#define TP_TAB5_CURRENT_SIGN      (-1.0)

/* ---------------------------------------------------------------------------
 * State of charge from 2S pack voltage.
 * PROVISIONAL generic Li-ion resting-voltage curve; replace with the Phase 0
 * measured discharge curve of the NP-F550 pack.
 * ------------------------------------------------------------------------- */
typedef struct {
    float pack_v;  /* strictly decreasing across the table */
    float pct;
} tp_soc_point_t;

/* Look up SoC (0..100) from pack voltage using the built-in provisional table. */
float tp_soc_from_pack_voltage(float pack_v);

/* Same, with internal-resistance compensation: v_ocv = v_meas - i * r_int
 * (i in A, + = charging). r_int_ohm = 0 disables compensation. */
float tp_soc_from_pack_voltage_comp(float pack_v, float current_a, float r_int_ohm);

/* Generic table lookup (table sorted by descending voltage, n >= 2). */
float tp_soc_lookup(const tp_soc_point_t *table, int n, float pack_v);

/* Access to the built-in table (for diagnostics UI / tests). */
const tp_soc_point_t *tp_soc_table(int *n_out);

/* ---------------------------------------------------------------------------
 * Low-battery state machine (build plan Section 3.1).
 *   OK -> WARN (<20 %) -> BLOCK_NEW_TEST (<10 %) -> SHUTDOWN (<5 %)
 * Escalate after `n_consecutive` valid readings below a threshold; the new
 * state is the least severe level all those readings agree on.
 * De-escalate only when the reading is >= threshold + hysteresis for
 * `n_consecutive` readings. SHUTDOWN is latched (call tp_lowbat_reset()).
 * While charging, escalation is suppressed (de-escalation still allowed).
 * ------------------------------------------------------------------------- */
typedef enum {
    TP_LOWBAT_OK = 0,
    TP_LOWBAT_WARN = 1,
    TP_LOWBAT_BLOCK_NEW_TEST = 2,
    TP_LOWBAT_SHUTDOWN = 3
} tp_lowbat_state_t;

typedef struct {
    float warn_pct;       /* default 20 */
    float block_pct;      /* default 10 */
    float shutdown_pct;   /* default 5 */
    float hysteresis_pct; /* default 2 */
    int n_consecutive;    /* default 3 */
} tp_lowbat_cfg_t;

typedef struct {
    tp_lowbat_cfg_t cfg;
    tp_lowbat_state_t state;
    int esc_count;
    tp_lowbat_state_t esc_level;  /* least severe candidate during current escalation run */
    int rec_count;
    tp_lowbat_state_t rec_level;  /* most severe candidate during current recovery run */
} tp_lowbat_t;

#define TP_LOWBAT_CFG_DEFAULT { 20.0f, 10.0f, 5.0f, 2.0f, 3 }

/* cfg may be NULL for defaults. Returns TP_ERR_ARG if thresholds are not
 * strictly decreasing / positive or n_consecutive < 1. */
int tp_lowbat_init(tp_lowbat_t *sm, const tp_lowbat_cfg_t *cfg);
void tp_lowbat_reset(tp_lowbat_t *sm);

/* Feed one reading. `valid` = false (sensor error) breaks any pending run.
 * Returns the (possibly new) state. */
tp_lowbat_state_t tp_lowbat_update(tp_lowbat_t *sm, float soc_pct, bool valid, bool charging);

const char *tp_lowbat_state_name(tp_lowbat_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* TAB5_POWER_CORE_H */
