/* Host tests for components/tab5_power/core */
#include "tinytest.h"
#include "tab5_power_core.h"

#include <math.h>

static void test_config_word(void)
{
    /* M5Unified Tab5 config: Rate16, 1.1 ms, 1.1 ms, ShuntAndBus continuous -> 0x0527 */
    TT_EQ_INT(tp_ina226_config_word(TP_INA226_AVG_16, TP_INA226_CT_1100US, TP_INA226_CT_1100US,
                                    TP_INA226_MODE_SHUNT_BUS_CONT), 0x0527);
    /* datasheet power-on default fields (AVG1, 1.1 ms, 1.1 ms, mode 7) -> 0x0127 (+ reserved 0x4000) */
    TT_EQ_INT(tp_ina226_config_word(TP_INA226_AVG_1, TP_INA226_CT_1100US, TP_INA226_CT_1100US,
                                    TP_INA226_MODE_SHUNT_BUS_CONT), 0x0127);
    TT_EQ_INT(tp_ina226_config_word(TP_INA226_AVG_1024, TP_INA226_CT_8244US, TP_INA226_CT_8244US,
                                    TP_INA226_MODE_SHUNT_BUS_CONT), 0x0FFF);
    TT_EQ_INT(tp_ina226_config_word(TP_INA226_AVG_1, TP_INA226_CT_140US, TP_INA226_CT_140US,
                                    TP_INA226_MODE_POWER_DOWN), 0x0000);
}

static void test_calibrate(void)
{
    tp_ina226_cal_t c;
    /* project default: 5 mOhm, 4.096 A -> LSB 125 uA, CAL 8192 exactly */
    TT_EQ_INT(tp_ina226_calibrate(TP_TAB5_SHUNT_OHM, TP_TAB5_MAX_CURRENT_A, &c), TP_OK);
    TT_EQ_INT(c.cal, 8192);
    TT_NEAR(c.current_lsb_a, 125e-6, 1e-12);
    TT_NEAR(c.power_lsb_w, 3.125e-3, 1e-12);

    /* M5Unified values: 5 mOhm, 2.0 A -> CAL trunc(16777.216) = 16777 */
    TT_EQ_INT(tp_ina226_calibrate(0.005, 2.0, &c), TP_OK);
    TT_EQ_INT(c.cal, 16777);
    TT_NEAR(c.current_lsb_a, 0.00512 / (16777.0 * 0.005), 1e-15);

    /* UserDemo inputs (5 mOhm, 8.192 A) with exact LSB -> CAL 4096 */
    TT_EQ_INT(tp_ina226_calibrate(0.005, 8.192, &c), TP_OK);
    TT_EQ_INT(c.cal, 4096);

    /* errors */
    TT_EQ_INT(tp_ina226_calibrate(0.0, 2.0, &c), TP_ERR_ARG);
    TT_EQ_INT(tp_ina226_calibrate(0.005, -1.0, &c), TP_ERR_ARG);
    TT_EQ_INT(tp_ina226_calibrate(0.005, 2.0, NULL), TP_ERR_ARG);
    TT_EQ_INT(tp_ina226_calibrate(0.005, 0.5, &c), TP_ERR_RANGE);    /* CAL 67108 > 0x7FFF */
    TT_EQ_INT(tp_ina226_calibrate(100.0, 100.0, &c), TP_ERR_RANGE);  /* CAL < 1 */
}

static void test_conversions(void)
{
    tp_ina226_cal_t c;
    TT_EQ_INT(tp_ina226_calibrate(TP_TAB5_SHUNT_OHM, TP_TAB5_MAX_CURRENT_A, &c), TP_OK);

    /* bus: 7.4 V = 5920 counts */
    TT_NEAR(tp_ina226_bus_volts(5920), 7.4, 1e-9);
    TT_NEAR(tp_ina226_bus_volts(0x7FFF), 40.95875, 1e-9);
    TT_NEAR(tp_ina226_bus_volts(0x8000), 0.0, 1e-12); /* bit 15 ignored */

    /* shunt: signed */
    TT_NEAR(tp_ina226_shunt_volts(1000), 2.5e-3, 1e-12);
    TT_NEAR(tp_ina226_shunt_volts((uint16_t)-1000), -2.5e-3, 1e-12);
    TT_NEAR(tp_ina226_shunt_volts(0x7FFF), 81.9175e-3, 1e-9);

    /* current: 0.5 A discharge on 5 mOhm = 2.5 mV = 1000 shunt counts;
       CURRENT reg = shunt * CAL / 2048 = 1000*8192/2048 = 4000 counts * 125 uA = 0.5 A */
    TT_NEAR(tp_ina226_current_amps(4000, &c), 0.5, 1e-9);
    TT_NEAR(tp_ina226_current_amps((uint16_t)-4000, &c), -0.5, 1e-9);
    TT_NEAR(tp_ina226_current_from_shunt(1000, 0.005), 0.5, 1e-9);
    TT_NEAR(TP_TAB5_CURRENT_SIGN * tp_ina226_current_amps(4000, &c), -0.5, 1e-9); /* discharge -> negative */

    /* power: 3.7 W / 3.125 mW = 1184 counts */
    TT_NEAR(tp_ina226_power_watts(1184, &c), 3.7, 1e-9);
    TT_NEAR(tp_ina226_power_watts(0xFFFF, &c), 65535 * 3.125e-3, 1e-9); /* unsigned */

    TT_NEAR(tp_ina226_current_amps(100, NULL), 0.0, 0);
    TT_NEAR(tp_ina226_power_watts(100, NULL), 0.0, 0);
}

static void test_soc(void)
{
    int n = 0;
    const tp_soc_point_t *t = tp_soc_table(&n);
    TT_CHECK(n >= 2);
    for (int i = 1; i < n; i++) {
        TT_CHECK(t[i].pack_v < t[i - 1].pack_v);
        TT_CHECK(t[i].pct <= t[i - 1].pct);
    }
    TT_NEAR(tp_soc_from_pack_voltage(9.0f), 100.0, 1e-6);
    TT_NEAR(tp_soc_from_pack_voltage(8.40f), 100.0, 1e-6);
    TT_NEAR(tp_soc_from_pack_voltage(7.64f), 50.0, 1e-4);
    TT_NEAR(tp_soc_from_pack_voltage(7.46f), 20.0, 1e-4);
    TT_NEAR(tp_soc_from_pack_voltage(7.36f), 10.0, 1e-4);
    TT_NEAR(tp_soc_from_pack_voltage(7.20f), 5.0, 1e-4);
    TT_NEAR(tp_soc_from_pack_voltage(6.0f), 0.0, 1e-6);
    TT_NEAR(tp_soc_from_pack_voltage(NAN), 0.0, 1e-6);
    /* midpoint interpolation between 7.46 (20) and 7.54 (30) */
    TT_NEAR(tp_soc_from_pack_voltage(7.50f), 25.0, 1e-3);
    /* monotonic sweep */
    float prev = -1.0f;
    for (float v = 6.0f; v <= 8.6f; v += 0.01f) {
        float s = tp_soc_from_pack_voltage(v);
        TT_CHECK(s >= prev - 1e-4f);
        TT_CHECK(s >= 0.0f && s <= 100.0f);
        prev = s;
    }
    /* IR compensation: -1 A discharge, 0.1 ohm -> OCV 0.1 V higher */
    TT_NEAR(tp_soc_from_pack_voltage_comp(7.54f, -1.0f, 0.1f), tp_soc_from_pack_voltage(7.64f), 1e-3);
    TT_NEAR(tp_soc_from_pack_voltage_comp(7.54f, -1.0f, 0.0f), tp_soc_from_pack_voltage(7.54f), 1e-6);

    /* generic lookup */
    const tp_soc_point_t two[] = { { 2.0f, 100.0f }, { 1.0f, 0.0f } };
    TT_NEAR(tp_soc_lookup(two, 2, 1.5f), 50.0, 1e-4);
    TT_NEAR(tp_soc_lookup(two, 1, 1.5f), 0.0, 0);
    TT_NEAR(tp_soc_lookup(NULL, 2, 1.5f), 0.0, 0);
}

static void feed(tp_lowbat_t *sm, float soc, int times, bool charging)
{
    for (int i = 0; i < times; i++) {
        tp_lowbat_update(sm, soc, true, charging);
    }
}

static void test_lowbat_basic(void)
{
    tp_lowbat_t sm;
    TT_EQ_INT(tp_lowbat_init(&sm, NULL), TP_OK);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);

    /* two low readings are not enough (N = 3) */
    feed(&sm, 19.0f, 2, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
    /* a good reading breaks the run */
    feed(&sm, 50.0f, 1, false);
    feed(&sm, 19.0f, 2, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
    feed(&sm, 19.0f, 1, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);

    /* inside hysteresis band (20..22): stays WARN */
    feed(&sm, 21.5f, 10, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);
    /* recovery needs >= 22 for 3 readings */
    feed(&sm, 22.0f, 2, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);
    feed(&sm, 22.0f, 1, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);

    /* down to block */
    feed(&sm, 9.0f, 3, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_BLOCK_NEW_TEST);
    TT_EQ_STR(tp_lowbat_state_name(sm.state), "BLOCK_NEW_TEST");
    /* 11 % is inside block hysteresis (10..12): stays blocked */
    feed(&sm, 11.0f, 5, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_BLOCK_NEW_TEST);
    /* 15 % -> WARN after 3 */
    feed(&sm, 15.0f, 3, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);

    /* shutdown, latched */
    feed(&sm, 4.0f, 3, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_SHUTDOWN);
    feed(&sm, 80.0f, 10, true);
    TT_EQ_INT(sm.state, TP_LOWBAT_SHUTDOWN);
    tp_lowbat_reset(&sm);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
}

static void test_lowbat_jump_and_noise(void)
{
    tp_lowbat_t sm;
    TT_EQ_INT(tp_lowbat_init(&sm, NULL), TP_OK);

    /* boot at 3 %: jumps straight to SHUTDOWN after N readings */
    feed(&sm, 3.0f, 3, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_SHUTDOWN);

    /* mixed run 8, 4, 15 -> least severe agreed level is WARN */
    tp_lowbat_reset(&sm);
    tp_lowbat_update(&sm, 8.0f, true, false);
    tp_lowbat_update(&sm, 4.0f, true, false);
    tp_lowbat_update(&sm, 15.0f, true, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);

    /* a single spike below 5 % does not shut down */
    tp_lowbat_reset(&sm);
    feed(&sm, 30.0f, 3, false);
    tp_lowbat_update(&sm, 2.0f, true, false);
    feed(&sm, 30.0f, 3, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);

    /* invalid readings break a run */
    tp_lowbat_update(&sm, 4.0f, true, false);
    tp_lowbat_update(&sm, 4.0f, true, false);
    tp_lowbat_update(&sm, 0.0f, false, false);
    tp_lowbat_update(&sm, 4.0f, true, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
    tp_lowbat_update(&sm, NAN, true, false);
    tp_lowbat_update(&sm, 4.0f, true, false);
    tp_lowbat_update(&sm, 4.0f, true, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
    tp_lowbat_update(&sm, 4.0f, true, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_SHUTDOWN);

    /* recovery uses the most severe level of the run */
    tp_lowbat_reset(&sm);
    feed(&sm, 7.0f, 3, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_BLOCK_NEW_TEST);
    tp_lowbat_update(&sm, 50.0f, true, true);
    tp_lowbat_update(&sm, 13.0f, true, true);
    tp_lowbat_update(&sm, 50.0f, true, true);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);
}

static void test_lowbat_charging(void)
{
    tp_lowbat_t sm;
    TT_EQ_INT(tp_lowbat_init(&sm, NULL), TP_OK);
    /* charging suppresses escalation */
    feed(&sm, 4.0f, 10, true);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
    /* charging run then discharging run: must restart count */
    feed(&sm, 15.0f, 2, true);
    feed(&sm, 15.0f, 2, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
    feed(&sm, 15.0f, 1, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);
}

static void test_lowbat_cfg(void)
{
    tp_lowbat_t sm;
    tp_lowbat_cfg_t bad = { 10.0f, 20.0f, 5.0f, 2.0f, 3 };
    TT_EQ_INT(tp_lowbat_init(&sm, &bad), TP_ERR_ARG);
    tp_lowbat_cfg_t bad2 = { 20.0f, 10.0f, 5.0f, 2.0f, 0 };
    TT_EQ_INT(tp_lowbat_init(&sm, &bad2), TP_ERR_ARG);
    TT_EQ_INT(tp_lowbat_init(NULL, NULL), TP_ERR_ARG);
    tp_lowbat_cfg_t one = { 20.0f, 10.0f, 5.0f, 0.0f, 1 };
    TT_EQ_INT(tp_lowbat_init(&sm, &one), TP_OK);
    tp_lowbat_update(&sm, 19.9f, true, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_WARN);
    tp_lowbat_update(&sm, 20.0f, true, false);
    TT_EQ_INT(sm.state, TP_LOWBAT_OK);
    TT_EQ_STR(tp_lowbat_state_name(TP_LOWBAT_OK), "OK");
    TT_EQ_STR(tp_lowbat_state_name(TP_LOWBAT_SHUTDOWN), "SHUTDOWN");
}

int main(void)
{
    TT_RUN(test_config_word);
    TT_RUN(test_calibrate);
    TT_RUN(test_conversions);
    TT_RUN(test_soc);
    TT_RUN(test_lowbat_basic);
    TT_RUN(test_lowbat_jump_and_noise);
    TT_RUN(test_lowbat_charging);
    TT_RUN(test_lowbat_cfg);
    return tt_summary();
}
