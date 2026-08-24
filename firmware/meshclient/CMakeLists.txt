# meshclient — embedded Meshtastic client library (framing, want_config
# handshake, nodeDB, messaging) over a transport vtable. Extraction-grade:
# no includes from core/ or app/ (see docs/specs/S03-meshclient.md AC7).

# ---------------------------------------------------------------------------
# S03: generated nanopb C for meshtastic/protobufs (committed under proto/,
# regenerated via tools/gen_nanopb.sh — see that script for the pinned
# commits). Vendored/generated code: not ours, no -Wall -Wextra -Werror.
# ---------------------------------------------------------------------------
add_library(ff-meshclient-proto STATIC
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
target_include_directories(ff-meshclient-proto PUBLIC proto)
target_link_libraries(ff-meshclient-proto PUBLIC ff-nanopb)

# ---------------------------------------------------------------------------
# S03: the library itself.
# ---------------------------------------------------------------------------
add_library(ff-meshclient STATIC
    src/mc_framing.c
    src/mc_client.c
    src/mc_transport_tcp.c
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
endif()
