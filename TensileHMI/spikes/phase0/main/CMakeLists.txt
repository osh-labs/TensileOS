idf_component_register(
    SRCS "main.c" "p0_config.c" "p0_ui.c" "p0_tests_basic.c" "p0_tests_wifi.c" "p0_tests_components.c" "p0_tests_power.c" "p0_tests_scale.c"
    INCLUDE_DIRS "."
    REQUIRES json nvs_flash esp_app_format esp_wifi esp_netif esp_event esp_timer fatfs vfs esp_driver_i2c lwip
             tab5_power tensile_link tensile_upload tensile_report
)
