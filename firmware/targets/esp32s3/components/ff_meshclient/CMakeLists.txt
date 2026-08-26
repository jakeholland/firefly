# ff_meshclient — ESP-IDF wrapper for firmware/meshclient. Mirrors
# firmware/meshclient/CMakeLists.txt's ff-meshclient target with ONE
# deliberate difference: mc_transport_tcp.c is NOT built here.
#
# mc_transport_tcp.c (firmware/meshclient/src/mc_transport_tcp.c) is a
# POSIX sockets transport (<sys/socket.h>, <sys/select.h>, <unistd.h>) —
# it is how the sim target dials meshtasticd over TCP, and it does not
# exist under ESP-IDF/newlib at all. It was never meant to ship on
# device: S15's real transport is UART (slice c, mc_transport_t wired to
# UART0/UART1 GPIO43/44 per docs/specs/S15-esp32s3-target.md). Excluding
# it here is not a workaround for a portability bug — it is the correct,
# permanent shape of the device build; a POSIX TCP client was never going
# to be linked into firmware. Noted as a REAL FINDING in the S15a PR body
# anyway, since "which of ff-meshclient's three sources is host-only" is
# exactly the kind of thing this slice exists to surface.
set(FF_FW_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(FF_MC_DIR ${FF_FW_DIR}/meshclient)

idf_component_register(
    SRCS
        "${FF_MC_DIR}/src/mc_framing.c"
        "${FF_MC_DIR}/src/mc_client.c"
    INCLUDE_DIRS "${FF_MC_DIR}/include"
    REQUIRES ff_platform
    PRIV_REQUIRES ff_meshclient_proto
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
