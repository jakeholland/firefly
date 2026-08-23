# festpack — festpack.json subset parser (spec: fest-almanac v0.1).
#
# Skeleton only (S13/S14 walking skeleton). Real implementation arrives
# with S05; this INTERFACE target reserves the module's place in the
# superbuild and its include/ path for downstream consumers.

add_library(ff-festpack INTERFACE)
target_include_directories(ff-festpack INTERFACE include)
