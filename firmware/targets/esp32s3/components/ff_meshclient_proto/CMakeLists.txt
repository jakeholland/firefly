# ff_meshclient_proto — generated nanopb C for meshtastic/protobufs,
# committed under firmware/meshclient/proto/ (regenerated via
# meshclient/tools/gen_nanopb.sh). Vendored/generated code: not ours, no
# -Wall -Wextra -Werror. Mirrors firmware/meshclient/CMakeLists.txt's
# ff-meshclient-proto target file-for-file — both now read
# firmware/meshclient/sources.cmake's FF_MESHCLIENT_PROTO_SOURCES instead
# of hand-copying the list (see that file's header).
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_MC_PROTO_DIR ${FF_FW_DIR}/meshclient/proto)
set(FF_MC_DIR ${FF_FW_DIR}/meshclient)

include(${FF_MC_DIR}/sources.cmake)

set(FF_MESHCLIENT_PROTO_SOURCES_ABS ${FF_MESHCLIENT_PROTO_SOURCES})
list(TRANSFORM FF_MESHCLIENT_PROTO_SOURCES_ABS PREPEND "${FF_MC_DIR}/")

idf_component_register(
    SRCS ${FF_MESHCLIENT_PROTO_SOURCES_ABS}
    INCLUDE_DIRS "${FF_MC_PROTO_DIR}"
    REQUIRES ff_nanopb
)
