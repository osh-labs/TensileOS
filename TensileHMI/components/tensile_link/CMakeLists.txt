# tensile_link: USB serial link to the OpenScale (TensileOS firmware v1.6).
# core/ is pure C (host-tested in TensileHMI/test/host); tensile_link_esp.c is the ESP-IDF glue.
idf_component_register(
    SRCS
        "core/tl_line.c"
        "core/tl_parse.c"
        "core/tl_proto.c"
        "tensile_link_esp.c"
    INCLUDE_DIRS "include" "core"
    REQUIRES freertos
    PRIV_REQUIRES usb esp_timer log
)
