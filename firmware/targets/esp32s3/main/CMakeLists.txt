# S20 — demo mode embeds the festpack bytes (CONFIG_FF_DEMO_MODE). The
# EMBED_FILES symbol derives from the filename:
# _binary_firefly_fields_festpack_json_{start,end}. Embedded unconditionally
# (a few KB of rodata); app_main only parses it when CONFIG_FF_DEMO_MODE is
# set. Path is relative to this component dir, up to firmware/assets/demo/.
set(FF_DEMO_EMBED "${CMAKE_CURRENT_LIST_DIR}/../../../assets/demo/firefly-fields.festpack.json")

idf_component_register(
    SRCS
        "app_main.c"
        "ff_face.c"     # device face dispatch (S15b) — see ff_face.h
    INCLUDE_DIRS "."
    # ff_app: ff_shell.h + ff_intent.h + ff_demo.h (S20). ff_app_ui: the
    # scr_*.h builders ff_face.c calls. ff_display: the S15b display/touch
    # HAL. lvgl: app_main touches lv_screen_active/lv_obj_clean directly.
    REQUIRES ff_app ff_app_ui ff_display lvgl
    PRIV_REQUIRES esp_timer freertos
    EMBED_FILES "${FF_DEMO_EMBED}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
