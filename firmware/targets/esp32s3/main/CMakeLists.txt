idf_component_register(
    SRCS
        "app_main.c"
        "ff_face.c"       # device face dispatch (S15b) — see ff_face.h
        "ff_nvs_store.c"  # S21 §4 — NVS-backed ff_store_t (persistence that sticks)
    INCLUDE_DIRS "."
    # ff_app: ff_shell.h + ff_intent.h. ff_app_ui: the scr_*.h builders
    # ff_face.c calls. ff_display: the S15b display/touch HAL. lvgl: app_main
    # touches lv_screen_active/lv_obj_clean directly on the dirty-rebuild.
    REQUIRES ff_app ff_app_ui ff_core ff_display lvgl
    # nvs_flash: the real settings store (S21 §4). esp_timer/freertos: clock
    # + task delays.
    PRIV_REQUIRES esp_timer freertos nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
