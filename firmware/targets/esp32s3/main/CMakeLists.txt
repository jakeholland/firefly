idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES ff_app
    PRIV_REQUIRES esp_timer freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
