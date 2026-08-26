# ff_meshclient_proto — generated nanopb C for meshtastic/protobufs,
# committed under firmware/meshclient/proto/ (regenerated via
# meshclient/tools/gen_nanopb.sh). Vendored/generated code: not ours, no
# -Wall -Wextra -Werror. Mirrors firmware/meshclient/CMakeLists.txt's
# ff-meshclient-proto target file-for-file.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_MC_PROTO_DIR ${FF_FW_DIR}/meshclient/proto)

idf_component_register(
    SRCS
        "${FF_MC_PROTO_DIR}/meshtastic/mesh.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/channel.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/config.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/device_ui.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/module_config.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/atak.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/portnums.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/telemetry.pb.c"
        "${FF_MC_PROTO_DIR}/meshtastic/xmodem.pb.c"
    INCLUDE_DIRS "${FF_MC_PROTO_DIR}"
    REQUIRES ff_nanopb
)
