/**
 * fuzz_ff_proto.c — fuzzes ff_proto_decode(), the Firefly private-protocol
 * decoder (FF_PORTNUM 269, [ver:1][type:1][body...], S04). Pure function,
 * no I/O, so this is about as direct as a fuzz harness gets: hand it raw
 * bytes and every declared invariant in ff_proto.h's doc comment
 * ("Decode is strict ... never reads outside buf[0..n)") gets checked by
 * ASan on every call.
 *
 * Also round-trips: whenever decode succeeds, re-encodes the RALLY/STATUS/
 * FLARE bodies (the only types with an encoder) and confirms decoding that
 * fresh encoding yields the same field values — catches any decode/encode
 * asymmetry a pure "does it crash" check would miss.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ff_proto.h"

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    ff_proto_msg_t out;
    int type = ff_proto_decode(data, size, &out);

    if (type == (int)FF_PROTO_TYPE_RALLY) {
        uint8_t buf[FF_PROTO_MAX_PAYLOAD];
        int n = ff_proto_encode_rally(buf, sizeof(buf), out.body.rally.pos, out.body.rally.name);
        if (n > 0) {
            ff_proto_msg_t rt;
            int rt_type = ff_proto_decode(buf, (size_t)n, &rt);
            if (rt_type != type) {
                __builtin_trap();
            }
            if (strcmp(rt.body.rally.name, out.body.rally.name) != 0) {
                __builtin_trap();
            }
        }
    } else if (type == (int)FF_PROTO_TYPE_STATUS) {
        uint8_t buf[FF_PROTO_MAX_PAYLOAD];
        int n = ff_proto_encode_status(buf, sizeof(buf), out.body.status.text);
        if (n > 0) {
            ff_proto_msg_t rt;
            int rt_type = ff_proto_decode(buf, (size_t)n, &rt);
            if (rt_type != type) {
                __builtin_trap();
            }
            if (strcmp(rt.body.status.text, out.body.status.text) != 0) {
                __builtin_trap();
            }
        }
    } else if (type == (int)FF_PROTO_TYPE_FLARE) {
        uint8_t buf[FF_PROTO_MAX_PAYLOAD];
        int n = ff_proto_encode_flare(buf, sizeof(buf), out.body.flare.dur_s);
        if (n > 0) {
            ff_proto_msg_t rt;
            int rt_type = ff_proto_decode(buf, (size_t)n, &rt);
            if (rt_type != type || rt.body.flare.dur_s != out.body.flare.dur_s) {
                __builtin_trap();
            }
        }
    }

    return 0;
}
