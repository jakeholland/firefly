/**
 * test_scr_crewcode_qr.c — 2026-09-15 amendment to A02 slice D
 * (docs/specs/S02-core-crew.md §D, `S02_AC14`): the SHOW CODE face's QR
 * must encode the bare crew code, not the `firefly://` deep link, and it
 * must stay QR version 1 (21x21 modules) doing it.
 *
 * Owner report 2026-09-15: the phone's scanner struggled with this
 * face's QR up close, only decoding from further away than a wearer
 * showing a puck across a tent has room for. Root cause: the 35-byte
 * deep link (`firefly://crew?v=1&code=FIRE-4K9M7X`) pushed LVGL's
 * `lv_qrcode` to QR version 3 (29x29 modules) in the face's fixed 170px
 * canvas — under 6px a module. The 11-byte bare canonical code alone
 * (`FIRE-4K9M7X`) is everything `CrewCode.parse` (A02 §1.2) needs, and
 * it fits QR version 1 (21x21 modules) in the same canvas.
 *
 * This is the "unit test that feeds the exact string to the same
 * encoder and checks version/size" this amendment's task brief asks for
 * in place of a host-side QR *decoder* — no `zbarimg` or Python
 * `pyzbar` is installed in this environment (checked: neither `which
 * zbarimg` nor `python3 -c "import pyzbar"` succeeds), so this drives
 * the identical LVGL-vendored `qrcodegen` encoder `scr_settings.c`'s
 * `lv_qrcode_update` calls, rather than decoding a rendered PNG back.
 *
 * `scr_settings.c`'s `_Static_assert(FF_CREWCODE_LEN <= 14u, ...)` pins
 * the same fact at compile time from the byte-capacity arithmetic alone;
 * this file cross-checks it against the ACTUAL encoder, which is the
 * thing that can drift from the arithmetic if LVGL ever changes how
 * `lv_qrcode_update` picks a mode or an ECC level (AGENTS.md's proxy-
 * check lesson — measure, don't reason harder).
 */
#include <string.h>

#include "unity.h"

/* qrcodegen.h guards its body on LV_USE_QRCODE and pulls in lvgl.h
 * itself via a path relative to its own location; the `lvgl` CMake
 * target exposes its source ROOT as a PUBLIC include dir (upstream
 * env_support/cmake/os_desktop.cmake), which is what makes this path
 * resolve — the same reason `lv_qrcode.c` itself reaches it as
 * "../../../lvgl.h". */
#include "src/libs/qrcode/qrcodegen.h"

#include "ff_crewcode.h"

void setUp(void) {}
void tearDown(void) {}

/* Test vector 1 from docs/specs/fixtures/A02-crew-codes.json — the same
 * code every shipped SHOW CODE screenshot and doc example uses. */
#define A02_CANONICAL_CODE "FIRE-4K9M7X"

/* ---------------------------------------------------------------------
 * The bare code — what the SHOW CODE face's QR encodes as of this
 * amendment — stays QR version 1 (21x21 modules) under the SAME BYTE
 * mode + ECC MEDIUM `lv_qrcode_update` always uses.
 * ------------------------------------------------------------------- */

static void A02_bare_crew_code_fits_qr_version_1(void)
{
    TEST_ASSERT_EQUAL_UINT(FF_CREWCODE_LEN, strlen(A02_CANONICAL_CODE));

    /* Mirrors lv_qrcode_update's own call exactly (lv_qrcode.c): BYTE
     * mode is implicit in qrcodegen_getMinFitVersion (it always sizes a
     * single BYTE segment), and MEDIUM is the only ECC level LVGL's
     * `lv_qrcode` ever asks for. */
    int const version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, strlen(A02_CANONICAL_CODE));

    TEST_ASSERT_EQUAL_INT(1, version);
    TEST_ASSERT_EQUAL_INT(21, qrcodegen_version2size(version));
}

/* Every valid crew code is exactly FF_CREWCODE_LEN bytes (A02 §1.1's
 * fixed 6-symbol shape), so the fit above does not depend on which code
 * — but pin that invariant here too, rather than trusting the one test
 * vector to stand for the whole codec. A generator that ever emitted a
 * variable-length code would defeat the whole point of this file. */
static void A02_every_generated_code_is_exactly_FF_CREWCODE_LEN_bytes(void)
{
    static uint32_t const bit_patterns[] = {0u, 1u, 0x3FFFFFFFu /* max 30-bit value */, 0x155555u};
    for (size_t i = 0; i < sizeof(bit_patterns) / sizeof(bit_patterns[0]); i++) {
        char code[FF_CREWCODE_LEN + 1u];
        TEST_ASSERT_TRUE(ff_crewcode_from_bits(bit_patterns[i], code));
        TEST_ASSERT_EQUAL_UINT(FF_CREWCODE_LEN, strlen(code));
        TEST_ASSERT_EQUAL_INT(1, qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, strlen(code)));
    }
}

/* ---------------------------------------------------------------------
 * Documentary — the PRE-amendment payload (the full deep link) is what
 * actually regressed to version 3. Recorded so a future reader does not
 * have to re-derive the "before" numbers by hand, and so a change that
 * accidentally put the QR back on `invite_url` is caught here too, not
 * only by the golden pixel-diff.
 * ------------------------------------------------------------------- */

static void A02_the_old_deep_link_payload_needed_qr_version_3(void)
{
    char url[FF_CREWCODE_URL_MAX];
    size_t const n = ff_crewcode_invite_url(A02_CANONICAL_CODE, NULL, url, sizeof(url));
    TEST_ASSERT_TRUE(n > 0u);
    TEST_ASSERT_EQUAL_STRING("firefly://crew?v=1&code=" A02_CANONICAL_CODE, url);

    int const version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, strlen(url));
    TEST_ASSERT_EQUAL_INT(3, version);
    TEST_ASSERT_EQUAL_INT(29, qrcodegen_version2size(version));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(A02_bare_crew_code_fits_qr_version_1);
    RUN_TEST(A02_every_generated_code_is_exactly_FF_CREWCODE_LEN_bytes);
    RUN_TEST(A02_the_old_deep_link_payload_needed_qr_version_3);
    return UNITY_END();
}
