# tab5_power: M5Stack Tab5 power path (USB-A 5V, IP2326 charger lines, INA226, power-off).
# The BSP dependency (espressif/m5stack_tab5) comes from idf_component.yml; the
# component manager adds it to this component's requirements automatically.
idf_component_register(
    SRCS "core/tp_core.c" "tab5_power_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "core"
    PRIV_REQUIRES esp_driver_i2c freertos log
)
