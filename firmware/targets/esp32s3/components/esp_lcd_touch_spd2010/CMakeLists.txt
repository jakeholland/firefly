# Firefly vendored esp_lcd_touch_spd2010 (patched for ESP-IDF 5.3 new
# i2c_master driver — see the note atop esp_lcd_touch_spd2010.c). A project
# component of this name SHADOWS the registry component, so this build uses
# the patched read path instead of the broken managed one; the managed
# dependency is therefore dropped from main/idf_component.yml.
#
# The upstream component pulls its ESP_LCD_TOUCH_SPD2010_VER_* macros from
# cmake_utilities' package_manager (cu_pkg_define_version). We don't vendor
# cmake_utilities just for a version string — define the macros directly so
# the "Touch panel create success, version X.Y.Z" log still builds. Keep
# these in sync with the vendored source's upstream release (2.0.1).
idf_component_register(
    SRCS "esp_lcd_touch_spd2010.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_lcd" "esp_lcd_touch"
    PRIV_REQUIRES "esp_timer"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_TOUCH_SPD2010_VER_MAJOR=2
    ESP_LCD_TOUCH_SPD2010_VER_MINOR=0
    ESP_LCD_TOUCH_SPD2010_VER_PATCH=1
)

# 2026-09-08 QA hardening, gate-fix on top of #239's original commit:
# `esp_lcd_panel_io_i2c_config_t.transaction_timeout_ms` (used by this
# header's ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG() macro, below) does not
# exist in every ESP-IDF this project builds against. It landed on the
# release/v5.3 branch AFTER the v5.3.5 tag (upstream commit
# 69c97183c3f, "feat(lcd): add configurable timeout for lcd i2c panel")
# and is not yet in ANY tagged ESP-IDF release, so ESP_IDF_VERSION alone
# can't distinguish the two cases — both report 5.3.5. Concretely: the
# maintainer's own local toolchain (source ~/esp/esp-idf/export.sh
# tracks release/v5.3 branch HEAD, not the tag) HAS the field; CI's
# esp32.yml pins the actual v5.3.5 release tag via espressif/esp-idf-ci-
# action, which does NOT. Detect it at configure time instead of
# assuming either way — check_struct_has_member compiles a real
# translation unit against this exact toolchain's real headers, so it's
# correct regardless of which side of that upstream backport the build
# is running against.
include(CheckStructHasMember)
check_struct_has_member("esp_lcd_panel_io_i2c_config_t" transaction_timeout_ms
    "esp_lcd_io_i2c.h"
    FF_ESP_LCD_I2C_HAS_TRANSACTION_TIMEOUT
    LANGUAGE C)
if(FF_ESP_LCD_I2C_HAS_TRANSACTION_TIMEOUT)
    # PUBLIC, not PRIVATE: ff_display.c (a different component, REQUIRES
    # this one) expands ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG() itself —
    # the guard in esp_lcd_touch_spd2010.h has to see the same define
    # there too, not just inside this component's own .c file.
    target_compile_definitions(${COMPONENT_LIB} PUBLIC FF_ESP_LCD_I2C_HAS_TRANSACTION_TIMEOUT=1)
endif()
