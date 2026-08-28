/**
 * ff_demo_festpack.h — the Firefly Fields demo festpack, embedded as a C
 * byte array for the sim's `--demo` mode.
 *
 * The definition (ff_demo_festpack_gen.c) is GENERATED at CMake configure
 * time from firmware/assets/demo/firefly-fields.festpack.json — the JSON
 * is the single source of truth; this array is never hand-edited. On
 * device the same bytes are provided by ESP-IDF's EMBED_FILES instead (see
 * targets/esp32s3), so ff_demo_seed itself stays byte-source-agnostic.
 *
 * Not NUL-terminated (fp_parse / ff_shell_load_pack are length-based).
 */
#ifndef FF_DEMO_FESTPACK_H
#define FF_DEMO_FESTPACK_H

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char ff_demo_festpack_json[];
extern const unsigned int ff_demo_festpack_json_len;

#ifdef __cplusplus
}
#endif

#endif /* FF_DEMO_FESTPACK_H */
