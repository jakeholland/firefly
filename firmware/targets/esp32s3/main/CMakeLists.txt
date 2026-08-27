idf_component_register(
    SRCS
        "app_main.c"
        "ff_face.c"     # device face dispatch (S15b) — see ff_face.h
    INCLUDE_DIRS "."
    # ff_app: ff_shell.h + ff_intent.h. ff_app_ui: the scr_*.h builders
    # ff_face.c calls. ff_display: the S15b display/touch HAL. lvgl: app_main
    # touches lv_screen_active/lv_obj_clean directly on the dirty-rebuild.
    REQUIRES ff_app ff_app_ui ff_core ff_display lvgl
    PRIV_REQUIRES esp_timer freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
