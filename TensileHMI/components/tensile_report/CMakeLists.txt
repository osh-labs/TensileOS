file(GLOB TR_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/core/*.c")

idf_component_register(
    SRCS "tensile_report_esp.c" ${TR_CORE_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "core"
    REQUIRES log
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
