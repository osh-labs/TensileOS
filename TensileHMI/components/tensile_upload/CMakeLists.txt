idf_component_register(
    SRCS
        "tensile_upload_esp.c"
        "core/tu_url.c"
        "core/tu_status.c"
        "core/tu_propfind.c"
        "core/tu_plan.c"
    INCLUDE_DIRS "include" "core"
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_timer
)
