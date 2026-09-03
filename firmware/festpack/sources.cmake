# firmware/festpack/sources.cmake — the ONE list of ff-festpack's C
# sources.
#
# Shared, verbatim, by:
#   - the sim build (firmware/festpack/CMakeLists.txt):
#       add_library(ff-festpack STATIC ${FF_FESTPACK_SOURCES})
#   - the esp32s3 IDF component
#     (firmware/targets/esp32s3/components/ff_festpack/CMakeLists.txt),
#     which was already file-for-file identical to the sim list before
#     this change — see that file's original header comment.
#
# Paths are relative to this directory (firmware/festpack/).
set(FF_FESTPACK_SOURCES
    src/fp_pack.c    # S05 — festpack.json subset parser
    src/ff_sched.c   # S07 — schedule engine (now-playing/next-starred/day-lineup/alarm)
    src/fp_t9words.c # S08 predictive-T9 addendum — festpack->words bridge
)
