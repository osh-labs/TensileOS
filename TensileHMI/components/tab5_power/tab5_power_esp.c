/*
 * tab5_power ESP-IDF glue (ESP-IDF v5.5, i2c_master driver, espressif/m5stack_tab5 >= 1.3.2).
 *
 * IO expander 0x44 (PI4IOE5V6408, BSP "expander1") pin map, from M5Unified
 * Power_Class.inl (board_M5Tab5) and M5Tab5-UserDemo m5stack_tab5.c:
 *   P0 WLAN_PWR_EN   out  (BSP_WIFI_EN)
 *   P3 USB5V_EN      out  (BSP_USB_EN)
 *   P4 PWROFF_PULSE  out
 *   P5 nCHG_QC_EN    out, active low
 *   P6 CHG_STAT      in
 *   P7 CHG_EN        out, high = charge enabled
 * INA226 at 0x41, shunt 5 mOhm.
 */
#include "tab5_power.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "tab5_power";

#define TP_PIN_WLAN_PWR_EN   IO_EXPANDER_PIN_NUM_0
#define TP_PIN_USB5V_EN      IO_EXPANDER_PIN_NUM_3
#define TP_PIN_PWROFF_PULSE  IO_EXPANDER_PIN_NUM_4
#define TP_PIN_NCHG_QC_EN    IO_EXPANDER_PIN_NUM_5
#define TP_PIN_CHG_STAT      IO_EXPANDER_PIN_NUM_6
#define TP_PIN_CHG_EN        IO_EXPANDER_PIN_NUM_7

#define TP_I2C_TIMEOUT_MS    50
#define TP_I2C_SPEED_HZ      400000
#define TP_CHARGE_DETECT_A   0.020f

static struct {
    bool inited;
    SemaphoreHandle_t lock;
    i2c_master_dev_handle_t ina;
    bool ina_ok;
    esp_io_expander_handle_t ioe1;
    tp_ina226_cal_t cal;
    float rint;
    tp_lowbat_t lowbat;
} s_tp;

/* ---------------------------------------------------------------- INA226 */

static esp_err_t ina_write16(uint8_t reg, uint16_t val)
{
    const uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(s_tp.ina, buf, sizeof(buf), TP_I2C_TIMEOUT_MS);
}

static esp_err_t ina_read16(uint8_t reg, uint16_t *val)
{
    uint8_t rx[2] = { 0 };
    esp_err_t err = i2c_master_transmit_receive(s_tp.ina, &reg, 1, rx, sizeof(rx), TP_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *val = (uint16_t)((rx[0] << 8) | rx[1]);
    }
    return err;
}

static esp_err_t ina_setup(double shunt_ohm, double max_current_a)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "BSP I2C bus not available");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_TAB5_INA226_ADDR,
        .scl_speed_hz = TP_I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_tp.ina), TAG, "add INA226");

    uint16_t mfg = 0, die = 0;
    if (ina_read16(TP_INA226_REG_MFG_ID, &mfg) != ESP_OK || ina_read16(TP_INA226_REG_DIE_ID, &die) != ESP_OK ||
        mfg != TP_INA226_MFG_ID || die != TP_INA226_DIE_ID) {
        ESP_LOGE(TAG, "INA226 not found at 0x%02x (mfg=0x%04x die=0x%04x)", TP_TAB5_INA226_ADDR, mfg, die);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_FALSE(tp_ina226_calibrate(shunt_ohm, max_current_a, &s_tp.cal) == TP_OK,
                        ESP_ERR_INVALID_ARG, TAG, "bad shunt/max current");

    /* Same averaging/conversion settings as M5Unified and UserDemo: 16 avg, 1.1 ms, 1.1 ms, continuous */
    const uint16_t cfg = tp_ina226_config_word(TP_INA226_AVG_16, TP_INA226_CT_1100US, TP_INA226_CT_1100US,
                                               TP_INA226_MODE_SHUNT_BUS_CONT);
    ESP_RETURN_ON_ERROR(ina_write16(TP_INA226_REG_CONFIG, cfg), TAG, "INA226 config");
    ESP_RETURN_ON_ERROR(ina_write16(TP_INA226_REG_CAL, s_tp.cal.cal), TAG, "INA226 cal");
    ESP_LOGI(TAG, "INA226 ok: config=0x%04x cal=%u lsb=%.1f uA", cfg, s_tp.cal.cal, s_tp.cal.current_lsb_a * 1e6);
    return ESP_OK;
}

/* --------------------------------------------------------- IO expander */

static esp_err_t ioe_out(uint32_t pin, bool level)
{
    esp_err_t ret = ESP_OK;
    /* Same sequence as bsp_feature_enable(): direction, level, then leave high-Z */
    ret |= esp_io_expander_set_dir(s_tp.ioe1, pin, IO_EXPANDER_OUTPUT);
    ret |= esp_io_expander_set_level(s_tp.ioe1, pin, level ? 1 : 0);
    ret |= esp_io_expander_set_output_mode(s_tp.ioe1, pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    return ret == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t ioe_in(uint32_t pin, esp_io_expander_pullupdown_t pull)
{
    esp_err_t ret = ESP_OK;
    ret |= esp_io_expander_set_dir(s_tp.ioe1, pin, IO_EXPANDER_INPUT);
    ret |= esp_io_expander_set_pullupdown(s_tp.ioe1, pin, pull);
    return ret == ESP_OK ? ESP_OK : ESP_FAIL;
}

static void lock(void)   { xSemaphoreTake(s_tp.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_tp.lock); }

/* ------------------------------------------------------------- public */

esp_err_t tp_init(const tp_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!s_tp.inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    const tp_config_t def = TP_CONFIG_DEFAULT();
    const tp_config_t *c = cfg ? cfg : &def;

    if (tp_lowbat_init(&s_tp.lowbat, &c->lowbat) != TP_OK) {
        ESP_LOGE(TAG, "invalid low-battery thresholds");
        return ESP_ERR_INVALID_ARG;
    }
    s_tp.rint = c->pack_rint_ohm;

    s_tp.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tp.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init");

    /* First creation of this handle resets the 0x44 expander (all pins input, outputs 0). */
    s_tp.ioe1 = bsp_io_expander1_init();
    ESP_RETURN_ON_FALSE(s_tp.ioe1 != NULL, ESP_FAIL, TAG, "IO expander 0x44 init failed");

    esp_err_t err = ESP_OK;
    err |= bsp_feature_enable(BSP_FEATURE_WIFI, c->wifi_power_on);
    err |= bsp_feature_enable(BSP_FEATURE_USB, c->usb_a_5v_on);
    err |= ioe_out(TP_PIN_PWROFF_PULSE, false);
    err |= ioe_in(TP_PIN_CHG_STAT, IO_EXPANDER_PULL_DOWN); /* UserDemo + ESPHome use pull-down; M5Unified none */
    /* UserDemo order: QC first, 50 ms, then charge enable */
    err |= ioe_out(TP_PIN_NCHG_QC_EN, !c->charge_qc_enable);
    vTaskDelay(pdMS_TO_TICKS(50));
    err |= ioe_out(TP_PIN_CHG_EN, c->charge_enable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO expander configuration failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "power path: usb_a_5v=%d charge=%d qc=%d wifi_pwr=%d",
             c->usb_a_5v_on, c->charge_enable, c->charge_qc_enable, c->wifi_power_on);

    s_tp.inited = true; /* expander functions usable even if the INA226 is missing */

    err = ina_setup(c->shunt_ohm, c->max_current_a);
    s_tp.ina_ok = (err == ESP_OK);
    return err;
}

esp_err_t tp_usb_a_5v_set(bool on)
{
    ESP_RETURN_ON_FALSE(s_tp.inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    lock();
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_USB, on);
    unlock();
    ESP_LOGI(TAG, "USB-A 5V %s (%s)", on ? "on" : "off", esp_err_to_name(err));
    return err;
}

esp_err_t tp_usb_a_5v_cycle(uint32_t off_ms)
{
    ESP_RETURN_ON_ERROR(tp_usb_a_5v_set(false), TAG, "usb off");
    vTaskDelay(pdMS_TO_TICKS(off_ms));
    return tp_usb_a_5v_set(true);
}

esp_err_t tp_charge_enable(bool on)
{
    ESP_RETURN_ON_FALSE(s_tp.inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    lock();
    esp_err_t err = esp_io_expander_set_level(s_tp.ioe1, TP_PIN_CHG_EN, on ? 1 : 0);
    unlock();
    return err;
}

esp_err_t tp_charge_qc_enable(bool on)
{
    ESP_RETURN_ON_FALSE(s_tp.inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    lock();
    esp_err_t err = esp_io_expander_set_level(s_tp.ioe1, TP_PIN_NCHG_QC_EN, on ? 0 : 1);
    unlock();
    return err;
}

esp_err_t tp_read(tp_reading_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out");
    ESP_RETURN_ON_FALSE(s_tp.inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    memset(out, 0, sizeof(*out));

    lock();
    esp_err_t err = s_tp.ina_ok ? ESP_OK : ESP_ERR_NOT_FOUND;
    uint16_t r_shunt = 0, r_bus = 0, r_pow = 0;
    if (err == ESP_OK) err = ina_read16(TP_INA226_REG_SHUNT_V, &r_shunt);
    if (err == ESP_OK) err = ina_read16(TP_INA226_REG_BUS_V, &r_bus);
    if (err == ESP_OK) err = ina_read16(TP_INA226_REG_POWER, &r_pow);

    uint32_t levels = 0;
    esp_err_t err_stat = esp_io_expander_get_level(s_tp.ioe1, TP_PIN_CHG_STAT, &levels);

    if (err == ESP_OK) {
        out->valid = true;
        out->bus_v = (float)tp_ina226_bus_volts(r_bus);
        out->shunt_mv = (float)(tp_ina226_shunt_volts(r_shunt) * 1000.0);
        /* Current from the shunt register: independent of CAL (survives an INA226 brown-out reset). */
        out->current_a = (float)(TP_TAB5_CURRENT_SIGN * tp_ina226_current_from_shunt(r_shunt, s_tp.cal.shunt_ohm));
        out->power_w = (float)tp_ina226_power_watts(r_pow, &s_tp.cal);
        out->soc_pct = tp_soc_from_pack_voltage_comp(out->bus_v, out->current_a, s_tp.rint);
    }
    out->chg_stat = (err_stat == ESP_OK) && ((levels & TP_PIN_CHG_STAT) != 0);
    out->charging = out->chg_stat || (out->valid && out->current_a > TP_CHARGE_DETECT_A);
    out->lowbat = tp_lowbat_update(&s_tp.lowbat, out->soc_pct, out->valid, out->charging);
    unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "INA226 read failed: %s", esp_err_to_name(err));
    }
    return err;
}

tp_lowbat_state_t tp_lowbat_state(void)
{
    if (!s_tp.inited) {
        return TP_LOWBAT_OK;
    }
    lock();
    tp_lowbat_state_t st = s_tp.lowbat.state;
    unlock();
    return st;
}

void tp_lowbat_clear(void)
{
    if (!s_tp.inited) {
        return;
    }
    lock();
    tp_lowbat_reset(&s_tp.lowbat);
    unlock();
}

esp_err_t tp_power_off(void)
{
    ESP_RETURN_ON_FALSE(s_tp.inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_LOGW(TAG, "power off");
    lock();
    /* UserDemo pulses 3x 100 ms; M5Unified toggles 10x 50 ms. Do 5x 100 ms high/low. */
    for (int i = 0; i < 5; i++) {
        esp_io_expander_set_level(s_tp.ioe1, TP_PIN_PWROFF_PULSE, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_io_expander_set_level(s_tp.ioe1, TP_PIN_PWROFF_PULSE, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    unlock();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "still running after power-off pulse (external power present?)");
    return ESP_FAIL;
}
