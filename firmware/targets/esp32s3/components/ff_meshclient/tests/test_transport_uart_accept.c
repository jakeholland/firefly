/**
 * test_transport_uart_accept.c — host-buildable unit test for
 * mc_uart_write_accept_len() (mc_transport_uart_accept.h), the one pure
 * calculation inside the UART transport's write() path (S15c,
 * docs/specs/S15-esp32s3-target.md slice c).
 *
 * The transport itself (mc_transport_uart.c) is device-only — it
 * #includes ESP-IDF's <driver/uart.h> and calls uart_driver_install()/
 * uart_get_tx_buffer_free_size()/uart_write_bytes(), none of which exist
 * off-device, so it cannot be built or exercised by this host gate (see
 * this PR's body, "Tests" section, for the honest statement of that
 * gap). mc_uart_write_accept_len() was deliberately factored out into a
 * header with ZERO ESP-IDF includes specifically so at least the
 * backpressure arithmetic — the part of this transport that must honour
 * the #170 contract (mc_client.h's mc_transport_t doc comment: write()
 * returns bytes accepted, 0 = try later, never blocks) — has a real,
 * host-run test rather than shipping unverified.
 *
 * Proving the #170 contract against mc_write_bytes()'s retry budget
 * (mc_client.c, MC_WRITE_ZERO_RETRY_BUDGET = 64 calls): this helper
 * returning 0 exactly reproduces "accepted nothing, try again" — the
 * budget-consuming case — and never returns a negative value itself (the
 * .c file's write() callback returns negative only from a genuine
 * uart_get_tx_buffer_free_size()/uart_write_bytes() driver error, never
 * from this pure function), so a transient TX-ring-full condition alone
 * can consume at most 64 mc_write_bytes() calls before mc_client gives up
 * — it can never look like a hard failure on its own.
 */
#include "unity.h"

#include "mc_transport_uart_accept.h"

void setUp(void) {}
void tearDown(void) {}

/* Plenty of free space, small request: the whole request is accepted,
 * unclamped. */
static void test_ample_free_space_accepts_full_request(void)
{
    TEST_ASSERT_EQUAL_UINT(64u, mc_uart_write_accept_len(4096u, 32u, 64u));
}

/* free_size <= margin: nothing is safe to send this call. Must read
 * exactly 0 — the #170 "try again later" case, not an error. */
static void test_free_size_at_or_below_margin_returns_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, mc_uart_write_accept_len(32u, 32u, 64u));  /* exactly equal */
    TEST_ASSERT_EQUAL_UINT(0u, mc_uart_write_accept_len(10u, 32u, 64u));  /* below */
    TEST_ASSERT_EQUAL_UINT(0u, mc_uart_write_accept_len(0u, 32u, 64u));   /* ring fully drained-of-space */
}

/* free_size just above margin, request larger than what's left: clamps
 * to (free_size - margin), the exact byte count uart_write_bytes() can
 * be asked for without blocking — never the raw free_size (which would
 * ignore the reserved header-item margin) and never the full request. */
static void test_partial_free_space_clamps_to_free_minus_margin(void)
{
    TEST_ASSERT_EQUAL_UINT(10u, mc_uart_write_accept_len(42u, 32u, 64u));
    TEST_ASSERT_EQUAL_UINT(1u, mc_uart_write_accept_len(33u, 32u, 64u)); /* boundary: 1 byte of real room */
}

/* Request smaller than available room: the request itself is the
 * accepted amount (never inflated up to the free size). */
static void test_request_smaller_than_available_is_not_inflated(void)
{
    TEST_ASSERT_EQUAL_UINT(5u, mc_uart_write_accept_len(4096u, 32u, 5u));
}

/* margin == 0 (the "disable the reservation" escape hatch this header
 * documents): behaves as a bare min(free_size, requested) clamp. */
static void test_zero_margin_is_a_bare_min_clamp(void)
{
    TEST_ASSERT_EQUAL_UINT(100u, mc_uart_write_accept_len(100u, 0u, 250u));
    TEST_ASSERT_EQUAL_UINT(250u, mc_uart_write_accept_len(1000u, 0u, 250u));
    TEST_ASSERT_EQUAL_UINT(0u, mc_uart_write_accept_len(0u, 0u, 250u));
}

/* requested == 0: always 0, regardless of how much room exists — an
 * empty write() call has nothing to accept. */
static void test_zero_requested_is_always_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, mc_uart_write_accept_len(4096u, 32u, 0u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ample_free_space_accepts_full_request);
    RUN_TEST(test_free_size_at_or_below_margin_returns_zero);
    RUN_TEST(test_partial_free_space_clamps_to_free_minus_margin);
    RUN_TEST(test_request_smaller_than_available_is_not_inflated);
    RUN_TEST(test_zero_margin_is_a_bare_min_clamp);
    RUN_TEST(test_zero_requested_is_always_zero);
    return UNITY_END();
}
