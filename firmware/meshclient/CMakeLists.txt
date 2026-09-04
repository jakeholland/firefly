# meshclient — embedded Meshtastic client library (framing, want_config
# handshake, nodeDB, messaging) over a transport vtable. Extraction-grade:
# no includes from core/ or app/ (see docs/specs/S03-meshclient.md AC7).
#
# Source lists live in sources.cmake, shared verbatim with the esp32s3
# IDF components ff_meshclient_proto and ff_meshclient
# (firmware/targets/esp32s3/components/) — see that file's header for why
# (including why mc_transport_tcp.c gets its own sim-only list).
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)

# Sim-only completeness guards (see ff_check_sources_complete()'s comment
# in the top-level firmware/CMakeLists.txt).
ff_check_sources_complete(${CMAKE_CURRENT_SOURCE_DIR}/proto/meshtastic ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake FF_MESHCLIENT_PROTO_SOURCES)
ff_check_sources_complete(${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake
    FF_MESHCLIENT_SOURCES FF_MESHCLIENT_SIM_ONLY_SOURCES)

# ---------------------------------------------------------------------------
# S03: generated nanopb C for meshtastic/protobufs (committed under proto/,
# regenerated via tools/gen_nanopb.sh — see that script for the pinned
# commits). Vendored/generated code: not ours, no -Wall -Wextra -Werror.
# ---------------------------------------------------------------------------
add_library(ff-meshclient-proto STATIC ${FF_MESHCLIENT_PROTO_SOURCES})
target_include_directories(ff-meshclient-proto PUBLIC proto)
target_link_libraries(ff-meshclient-proto PUBLIC ff-nanopb)

# ---------------------------------------------------------------------------
# S03: the library itself. mc_transport_tcp.c (FF_MESHCLIENT_SIM_ONLY_
# SOURCES) is how the sim dials meshtasticd over TCP; the device build
# excludes it — see sources.cmake's header comment.
# ---------------------------------------------------------------------------
add_library(ff-meshclient STATIC
    ${FF_MESHCLIENT_SOURCES}
    ${FF_MESHCLIENT_SIM_ONLY_SOURCES}
)
target_include_directories(ff-meshclient PUBLIC include)
target_link_libraries(ff-meshclient
    PUBLIC ff-platform
    PRIVATE ff-meshclient-proto
)
ff_apply_warnings(ff-meshclient)

if(FF_BUILD_TESTS)
    add_executable(test_meshclient
        tests/test_meshclient.c
    )
    target_compile_definitions(test_meshclient PRIVATE
        MC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        MC_MESHCLIENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(test_meshclient PRIVATE
        ff-meshclient
        ff-meshclient-proto
        unity
    )
    ff_apply_warnings(test_meshclient)

    add_test(NAME test_meshclient COMMAND test_meshclient)

    # S16 slice e: mc_tcp re-dial after a dropped connection. A real
    # loopback socket, deliberately — see test_transport_tcp.c's header
    # comment for why the mock-transport tests above can't catch this
    # class of bug at all (they have no fd to break in the first place).
    add_executable(test_transport_tcp
        tests/test_transport_tcp.c
    )
    target_link_libraries(test_transport_tcp PRIVATE
        ff-meshclient
        unity
    )
    ff_apply_warnings(test_transport_tcp)

    add_test(NAME test_transport_tcp COMMAND test_transport_tcp)

    # S15c — mc_uart_write_accept_len() (device UART transport's write()
    # backpressure arithmetic, the #170 contract). The transport itself
    # (targets/esp32s3/components/ff_meshclient/mc_transport_uart.c) is
    # device-only (ESP-IDF <driver/uart.h>, no sim/host build exists for
    # it — see this PR's body for the honest gap statement), but the one
    # pure calculation inside its write() path was factored into a
    # header with zero ESP-IDF includes
    # (targets/esp32s3/components/ff_meshclient/include/
    # mc_transport_uart_accept.h) specifically so it has a real,
    # host-run test rather than shipping unverified. No link against
    # ff-meshclient: the header is self-contained (stddef.h only).
    add_executable(test_transport_uart_accept
        ${CMAKE_CURRENT_SOURCE_DIR}/../targets/esp32s3/components/ff_meshclient/tests/test_transport_uart_accept.c
    )
    target_include_directories(test_transport_uart_accept PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../targets/esp32s3/components/ff_meshclient/include
    )
    target_link_libraries(test_transport_uart_accept PRIVATE unity)
    ff_apply_warnings(test_transport_uart_accept)

    add_test(NAME test_transport_uart_accept COMMAND test_transport_uart_accept)
endif()
