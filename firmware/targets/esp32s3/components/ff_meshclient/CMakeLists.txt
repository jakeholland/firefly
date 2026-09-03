# ff_meshclient — ESP-IDF wrapper for firmware/meshclient. Mirrors
# firmware/meshclient/CMakeLists.txt's ff-meshclient target with ONE
# deliberate difference: mc_transport_tcp.c is NOT built here — both now
# read firmware/meshclient/sources.cmake's FF_MESHCLIENT_SOURCES (shared)
# instead of hand-copying the list, while FF_MESHCLIENT_SIM_ONLY_SOURCES
# stays sim-only by construction (this component never reads it).
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

include(${FF_MC_DIR}/sources.cmake)

set(FF_MESHCLIENT_SOURCES_ABS ${FF_MESHCLIENT_SOURCES})
list(TRANSFORM FF_MESHCLIENT_SOURCES_ABS PREPEND "${FF_MC_DIR}/")

idf_component_register(
    SRCS ${FF_MESHCLIENT_SOURCES_ABS}
    INCLUDE_DIRS "${FF_MC_DIR}/include"
    REQUIRES ff_platform
    PRIV_REQUIRES ff_meshclient_proto
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
