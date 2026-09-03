# firmware/meshclient/sources.cmake — the source lists for meshclient's
# libraries.
#
# Shared, verbatim, by:
#   - the sim build (firmware/meshclient/CMakeLists.txt): two targets,
#     ff-meshclient-proto (generated nanopb C) and ff-meshclient (the
#     library itself).
#   - the esp32s3 IDF components (firmware/targets/esp32s3/components/
#     ff_meshclient_proto/ and ff_meshclient/CMakeLists.txt).
#
# mc_transport_tcp.c is the ONE deliberate difference between the sim and
# the device: it's a POSIX sockets transport (<sys/socket.h>,
# <sys/select.h>, <unistd.h>) used only so the sim target can dial
# meshtasticd over TCP, and it does not exist under ESP-IDF/newlib at
# all — S15's real transport is UART. It gets its own list,
# FF_MESHCLIENT_SIM_ONLY_SOURCES, so the device component can leave it
# out while still sharing FF_MESHCLIENT_SOURCES (mc_framing.c/mc_client.c)
# with the sim — see ff_meshclient's IDF CMakeLists.txt comment for why
# excluding it there is correct, permanent shape, not a portability
# workaround.
#
# Paths are relative to this directory (firmware/meshclient/).

# S03 — generated nanopb C for meshtastic/protobufs (committed under
# proto/, regenerated via tools/gen_nanopb.sh). Vendored/generated code:
# not ours, no -Wall -Wextra -Werror in either consumer.
set(FF_MESHCLIENT_PROTO_SOURCES
    proto/meshtastic/mesh.pb.c
    proto/meshtastic/channel.pb.c
    proto/meshtastic/config.pb.c
    proto/meshtastic/device_ui.pb.c
    proto/meshtastic/module_config.pb.c
    proto/meshtastic/atak.pb.c
    proto/meshtastic/portnums.pb.c
    proto/meshtastic/telemetry.pb.c
    proto/meshtastic/xmodem.pb.c
)

# S03 — the library itself: framing + want_config handshake + nodeDB +
# messaging over a transport vtable. Built on BOTH targets.
set(FF_MESHCLIENT_SOURCES
    src/mc_framing.c
    src/mc_client.c
)

# Sim-only: the TCP transport (see this file's header comment above).
set(FF_MESHCLIENT_SIM_ONLY_SOURCES
    src/mc_transport_tcp.c
)
