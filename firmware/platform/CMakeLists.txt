# platform — tiny cross-cutting seams shared by extraction-grade libraries
# (currently just ff_clock_t). See firmware/platform/include/ff_clock.h.
#
# Header-only: INTERFACE target, same shape as ff-app/ff-festpack skeletons.

add_library(ff-platform INTERFACE)
target_include_directories(ff-platform INTERFACE include)
