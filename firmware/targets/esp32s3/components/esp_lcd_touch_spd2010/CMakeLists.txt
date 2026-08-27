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
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_LCD_TOUCH_SPD2010_VER_MAJOR=2
    ESP_LCD_TOUCH_SPD2010_VER_MINOR=0
    ESP_LCD_TOUCH_SPD2010_VER_PATCH=1
)
