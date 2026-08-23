# meshclient — embedded Meshtastic client library (framing, want_config
# handshake, nodeDB, messaging) over a transport vtable.
#
# Skeleton only (S13/S14 walking skeleton). Real implementation arrives
# with S03; this INTERFACE target reserves the module's place in the
# superbuild and its include/ path for downstream consumers.

add_library(ff-meshclient INTERFACE)
target_include_directories(ff-meshclient INTERFACE include)
