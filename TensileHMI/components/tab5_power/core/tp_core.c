/*
 * tab5_power core: INA226 math, 2S SoC estimate, low-battery state machine.
 * Pure C11, no ESP-IDF headers.
 */
#include "tab5_power_core.h"

#include <math.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* INA226                                                                     */
/* ------------------------------------------------------------------------- */

uint16_t tp_ina226_config_word(tp_ina226_avg_t avg, tp_ina226_ct_t vbus_ct,
                               tp_ina226_ct_t vsh_ct, tp_ina226_mode_t mode)
{
    return (uint16_t)((((unsigned)avg & 0x7u) << 9) |
                      (((unsigned)vbus_ct & 0x7u) << 6) |
                      (((unsigned)vsh_ct & 0x7u) << 3) |
                      ((unsigned)mode & 0x7u));
}

int tp_ina226_calibrate(double shunt_ohm, double max_current_a, tp_ina226_cal_t *out)
{
    if (out == NULL || !(shunt_ohm > 0.0) || !(max_current_a > 0.0)) {
        return TP_ERR_ARG;
    }
    const double lsb = max_current_a / 32768.0;
    const double cal_d = TP_INA226_CAL_CONST / (lsb * shunt_ohm);
    /* small epsilon so exact values (e.g. 8192.0 computed as 8191.99999) do not truncate down */
    const double cal_f = floor(cal_d + 1e-6);
    if (cal_f < 1.0 || cal_f > 32767.0) {
        return TP_ERR_RANGE;
    }
    out->cal = (uint16_t)cal_f;
    out->shunt_ohm = shunt_ohm;
    /* effective LSB the chip actually applies with the truncated CAL */
    out->current_lsb_a = TP_INA226_CAL_CONST / (cal_f * shunt_ohm);
    out->power_lsb_w = TP_INA226_POWER_LSB_MULT * out->current_lsb_a;
    return TP_OK;
}

double tp_ina226_bus_volts(uint16_t raw)
{
    return (double)(raw & 0x7FFFu) * TP_INA226_BUS_LSB_V;
}

double tp_ina226_shunt_volts(uint16_t raw)
{
    return (double)(int16_t)raw * TP_INA226_SHUNT_LSB_V;
}

double tp_ina226_current_amps(uint16_t raw, const tp_ina226_cal_t *cal)
{
    if (cal == NULL) {
        return 0.0;
    }
    return (double)(int16_t)raw * cal->current_lsb_a;
}

double tp_ina226_power_watts(uint16_t raw, const tp_ina226_cal_t *cal)
{
    if (cal == NULL) {
        return 0.0;
    }
    return (double)raw * cal->power_lsb_w;
}

double tp_ina226_current_from_shunt(uint16_t raw_shunt, double shunt_ohm)
{
    if (!(shunt_ohm > 0.0)) {
        return 0.0;
    }
    return tp_ina226_shunt_volts(raw_shunt) / shunt_ohm;
}

/* ------------------------------------------------------------------------- */
/* State of charge                                                            */
/* ------------------------------------------------------------------------- */

/*
 * PROVISIONAL: generic Li-ion (LCO/NMC) resting open-circuit voltage per cell,
 * doubled for the 2S NP-F550 pack. Not from a datasheet for this pack. The
 * Phase 0 discharge measurement (constant load, logged V/I over a full
 * discharge) replaces these points. Kept deliberately conservative at the low
 * end so the 5 % shutdown sits well above the pack protection cutoff.
 *
 * Cross-checks: M5Unified maps 3.30 V/cell -> 0 % and ~4.10-4.15 V/cell ->
 * 100 % linearly; ESPHome's Tab5 config maps 6.0 V -> 0 %, 8.23 V -> 100 %
 * linearly (see phase0_hw_notes.md).
 */
static const tp_soc_point_t s_soc_2s[] = {
    { 8.40f, 100.0f },
    { 8.30f,  95.0f },
    { 8.22f,  90.0f },
    { 8.04f,  80.0f },
    { 7.90f,  70.0f },
    { 7.74f,  60.0f },
    { 7.64f,  50.0f },
    { 7.58f,  40.0f },
    { 7.54f,  30.0f },
    { 7.46f,  20.0f },
    { 7.40f,  15.0f },
    { 7.36f,  10.0f },
    { 7.20f,   5.0f },
    { 6.90f,   2.0f },
    { 6.60f,   0.0f },
};

const tp_soc_point_t *tp_soc_table(int *n_out)
{
    if (n_out) {
        *n_out = (int)(sizeof(s_soc_2s) / sizeof(s_soc_2s[0]));
    }
    return s_soc_2s;
}

float tp_soc_lookup(const tp_soc_point_t *table, int n, float pack_v)
{
    if (table == NULL || n < 2 || isnan(pack_v)) {
        return 0.0f;
    }
    if (pack_v >= table[0].pack_v) {
        return table[0].pct;
    }
    if (pack_v <= table[n - 1].pack_v) {
        return table[n - 1].pct;
    }
    for (int i = 1; i < n; i++) {
        if (pack_v >= table[i].pack_v) {
            const float v_hi = table[i - 1].pack_v, p_hi = table[i - 1].pct;
            const float v_lo = table[i].pack_v, p_lo = table[i].pct;
            const float span = v_hi - v_lo;
            if (span <= 0.0f) {
                return p_lo;
            }
            return p_lo + (pack_v - v_lo) * (p_hi - p_lo) / span;
        }
    }
    return table[n - 1].pct;
}

float tp_soc_from_pack_voltage(float pack_v)
{
    int n = 0;
    const tp_soc_point_t *t = tp_soc_table(&n);
    return tp_soc_lookup(t, n, pack_v);
}

float tp_soc_from_pack_voltage_comp(float pack_v, float current_a, float r_int_ohm)
{
    float v = pack_v;
    if (r_int_ohm > 0.0f && !isnan(current_a)) {
        /* charging (+I) raises terminal voltage above OCV; discharging (-I) lowers it */
        v = pack_v - current_a * r_int_ohm;
    }
    return tp_soc_from_pack_voltage(v);
}

/* ------------------------------------------------------------------------- */
/* Low-battery state machine                                                  */
/* ------------------------------------------------------------------------- */

static tp_lowbat_state_t severity_for(const tp_lowbat_cfg_t *c, float soc)
{
    if (soc < c->shutdown_pct) return TP_LOWBAT_SHUTDOWN;
    if (soc < c->block_pct) return TP_LOWBAT_BLOCK_NEW_TEST;
    if (soc < c->warn_pct) return TP_LOWBAT_WARN;
    return TP_LOWBAT_OK;
}

static tp_lowbat_state_t recovery_level(const tp_lowbat_cfg_t *c, float soc)
{
    const float h = c->hysteresis_pct;
    if (soc >= c->warn_pct + h) return TP_LOWBAT_OK;
    if (soc >= c->block_pct + h) return TP_LOWBAT_WARN;
    if (soc >= c->shutdown_pct + h) return TP_LOWBAT_BLOCK_NEW_TEST;
    return TP_LOWBAT_SHUTDOWN;
}

int tp_lowbat_init(tp_lowbat_t *sm, const tp_lowbat_cfg_t *cfg)
{
    static const tp_lowbat_cfg_t def = TP_LOWBAT_CFG_DEFAULT;
    if (sm == NULL) {
        return TP_ERR_ARG;
    }
    const tp_lowbat_cfg_t *c = cfg ? cfg : &def;
    if (!(c->warn_pct > c->block_pct) || !(c->block_pct > c->shutdown_pct) ||
        !(c->shutdown_pct > 0.0f) || c->hysteresis_pct < 0.0f || c->n_consecutive < 1) {
        return TP_ERR_ARG;
    }
    sm->cfg = *c;
    tp_lowbat_reset(sm);
    return TP_OK;
}

void tp_lowbat_reset(tp_lowbat_t *sm)
{
    if (sm == NULL) {
        return;
    }
    sm->state = TP_LOWBAT_OK;
    sm->esc_count = 0;
    sm->esc_level = TP_LOWBAT_OK;
    sm->rec_count = 0;
    sm->rec_level = TP_LOWBAT_OK;
}

tp_lowbat_state_t tp_lowbat_update(tp_lowbat_t *sm, float soc_pct, bool valid, bool charging)
{
    if (sm == NULL) {
        return TP_LOWBAT_OK;
    }
    if (!valid || isnan(soc_pct)) {
        sm->esc_count = 0;
        sm->rec_count = 0;
        return sm->state;
    }
    if (sm->state == TP_LOWBAT_SHUTDOWN) {
        return sm->state; /* latched: the app is saving and powering off */
    }

    const tp_lowbat_state_t cand = severity_for(&sm->cfg, soc_pct);
    if (cand > sm->state && !charging) {
        sm->rec_count = 0;
        if (sm->esc_count == 0 || cand < sm->esc_level) {
            sm->esc_level = cand;
        }
        if (++sm->esc_count >= sm->cfg.n_consecutive) {
            sm->state = sm->esc_level;
            sm->esc_count = 0;
        }
        return sm->state;
    }
    sm->esc_count = 0;

    const tp_lowbat_state_t rec = recovery_level(&sm->cfg, soc_pct);
    if (rec < sm->state) {
        if (sm->rec_count == 0 || rec > sm->rec_level) {
            sm->rec_level = rec;
        }
        if (++sm->rec_count >= sm->cfg.n_consecutive) {
            sm->state = sm->rec_level;
            sm->rec_count = 0;
        }
    } else {
        sm->rec_count = 0;
    }
    return sm->state;
}

const char *tp_lowbat_state_name(tp_lowbat_state_t s)
{
    switch (s) {
    case TP_LOWBAT_OK: return "OK";
    case TP_LOWBAT_WARN: return "WARN";
    case TP_LOWBAT_BLOCK_NEW_TEST: return "BLOCK_NEW_TEST";
    case TP_LOWBAT_SHUTDOWN: return "SHUTDOWN";
    default: return "?";
    }
}
