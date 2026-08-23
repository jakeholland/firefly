# tools — cross-cutting dev tooling (not a firmware module). S13/S14
# slice b: compare_png, the golden-screenshot pixel diff used by
# tests/run_goldens.sh.

# png_diff: the testable comparison core (see png_diff.h's header
# comment for why this is split from compare_png.c's CLI wrapper).
add_library(ff-png-diff STATIC
    png_diff.c
)
target_include_directories(ff-png-diff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
ff_apply_warnings(ff-png-diff)

if(FF_BUILD_TESTS)
    add_executable(test_png_diff
        tests/test_png_diff.c
    )
    target_link_libraries(test_png_diff PRIVATE ff-png-diff unity)
    ff_apply_warnings(test_png_diff)

    add_test(NAME test_png_diff COMMAND test_png_diff)
endif()

# compare_png — CLI wrapper: stb_image (read) + ff-png-diff (compare) +
# stb_image_write (diff artifact). Lands at build/compare_png, same
# RUNTIME_OUTPUT_DIRECTORY convention as ffsim (see targets/sim/CMakeLists.txt)
# so tests/run_goldens.sh can find both binaries the same way.
add_executable(compare_png
    compare_png.c
)
set_target_properties(compare_png PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(compare_png PRIVATE
    ff-png-diff
    ff-stb-image
    ff-stb-image-write
)
if(UNIX)
    # stb_image's PNG decode path uses zlib inflate math via libm on some
    # platforms (pow/floor et al pulled in by its Huffman/zlib routines).
    target_link_libraries(compare_png PRIVATE m)
endif()
ff_apply_warnings(compare_png)
