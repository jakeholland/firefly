# Vendored single-header libraries. Not FetchContent — small enough to
# check in directly, per S13. Pin/update by re-copying the upstream file
# and noting the source commit in the comment at the top of the file.

add_library(ff-stb-image-write INTERFACE)
target_include_directories(ff-stb-image-write INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# S05
add_library(ff-jsmn INTERFACE)
target_include_directories(ff-jsmn INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
