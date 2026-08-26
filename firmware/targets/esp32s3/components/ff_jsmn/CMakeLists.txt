# ff_jsmn — ESP-IDF wrapper for the vendored jsmn.h (firmware/third_party),
# header-only, same as the sim build's ff-jsmn INTERFACE target
# (firmware/third_party/CMakeLists.txt). Only ff_festpack needs this.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)

idf_component_register(
    INCLUDE_DIRS "${FF_FW_DIR}/third_party"
)
