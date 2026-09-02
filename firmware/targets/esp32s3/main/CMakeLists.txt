# S20 — demo mode embeds the festpack bytes (CONFIG_FF_DEMO_MODE). The
# EMBED_FILES symbol derives from the filename:
# _binary_firefly_fields_festpack_json_{start,end}. Embedded unconditionally
# (a few KB of rodata); app_main only parses it when CONFIG_FF_DEMO_MODE is
# set. Path is relative to this component dir, up to firmware/assets/demo/.
set(FF_DEMO_EMBED "${CMAKE_CURRENT_LIST_DIR}/../../../assets/demo/firefly-fields.festpack.json")

idf_component_register(
    SRCS
        "app_main.c"
        "ff_face.c"       # device face dispatch (S15b) — see ff_face.h
        "ff_nvs_store.c"  # S21 §4 — NVS-backed ff_store_t (persistence that sticks)
    INCLUDE_DIRS "."
    # ff_app: ff_shell.h + ff_intent.h + ff_demo.h (S20). ff_app_ui: the
    # scr_*.h builders ff_face.c calls. ff_core: core headers used directly.
    # ff_display: the S15b display/touch HAL. lvgl: app_main touches
    # lv_screen_active/lv_obj_clean directly on the dirty-rebuild.
    REQUIRES ff_app ff_app_ui ff_core ff_display ff_power lvgl
    # nvs_flash: the real settings store (S21 §4). esp_timer/freertos: clock
    # + task delays. esp_system: esp_restart() (S26 slice b's reboot action).
    # esp_driver_usb_serial_jtag: usb_serial_jtag_is_connected() (S26 slice f
    # amendment — light-sleep inhibit while USB is connected). Its connection
    # monitor is already linked in this project's sdkconfig
    # (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y forces the component's own
    # `-u usb_serial_jtag_connection_monitor_include` in
    # esp_driver_usb_serial_jtag/CMakeLists.txt) — no driver_install() call,
    # no sdkconfig change needed here.
    PRIV_REQUIRES esp_timer freertos nvs_flash esp_system esp_driver_usb_serial_jtag
    EMBED_FILES "${FF_DEMO_EMBED}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
