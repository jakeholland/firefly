/**
 * mc_transport_uart_accept.h — the ONE pure calculation inside
 * mc_transport_uart.c's write() path, pulled out so it can be unit
 * tested on the host build (this file has ZERO ESP-IDF includes —
 * it is plain C11, buildable by the sim's CMake tree exactly like any
 * other host-side header).
 *
 * Why this exists (S15c, docs/specs/S15-esp32s3-target.md slice c): the
 * UART transport's write() must honour the #170 contract stated in
 * mc_client.h's mc_transport_t doc comment — return the number of bytes
 * ACCEPTED, 0 meaning "try again later" (not an error), negative only on
 * a hard driver failure. ESP-IDF's uart_write_bytes() with a TX ring
 * buffer enabled (tx_buf_size > 0, required here for the >=1KB ring the
 * spec asks for) calls FreeRTOS's xRingbufferSend() with portMAX_DELAY
 * for any byte that doesn't fit — i.e. it BLOCKS THE CALLING TASK
 * (verified against esp-idf's components/esp_driver_uart/src/uart.c,
 * uart_tx_all()) rather than returning early, which would stall
 * mc_tick()'s shared UI tick exactly the way this transport must not.
 *
 * The fix mc_transport_uart.c applies: call uart_get_tx_buffer_free_size()
 * FIRST, then never ask uart_write_bytes() for more than what is already
 * known to fit — so the call it actually makes returns immediately
 * without blocking. mc_uart_write_accept_len() is that "how much can I
 * safely ask for" arithmetic, isolated from the ESP-IDF calls around it.
 *
 * `margin` exists because uart_tx_all() enqueues an internal
 * `uart_tx_data_t` header ITEM (via a separate xRingbufferSend() call,
 * see the .c file's own comment) before the payload bytes on every call —
 * that header consumes ring-buffer space and per-item overhead beyond
 * the payload itself, on top of what uart_get_tx_buffer_free_size()
 * already reserves for the NOSPLIT ring's own per-item header (see that
 * function's implementation, esp_driver_uart/src/uart.c). Reserving
 * `margin` bytes of the reported free size before computing the
 * acceptable payload keeps this call's own header item from silently
 * eating into space the caller believed was payload-safe.
 */
#ifndef MC_TRANSPORT_UART_ACCEPT_H
#define MC_TRANSPORT_UART_ACCEPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * mc_uart_write_accept_len — how many of `requested` bytes may be handed
 * to uart_write_bytes() this call without risking a block.
 *
 * @param free_size  bytes reported free by uart_get_tx_buffer_free_size()
 *                    just before this call (0 is a legal, common reading
 *                    — "ring full right now", not an error).
 * @param margin     bytes to reserve for this call's own internal
 *                    ring-buffer header item (see this header's top
 *                    comment). Pass 0 to disable the reservation
 *                    (useful for testing the bare clamp behaviour).
 * @param requested  bytes the caller (mc_client's mc_write_bytes) asked
 *                    this transport to accept.
 *
 * @return 0 if `free_size` does not exceed `margin` (nothing safe to
 *         send this call — the transport's write() must return 0, per
 *         the #170 contract, NOT an error); otherwise
 *         min(requested, free_size - margin).
 *
 * Pure function: no I/O, no globals, deterministic on its inputs alone.
 */
static inline size_t mc_uart_write_accept_len(size_t free_size, size_t margin, size_t requested)
{
    if (free_size <= margin) {
        return 0;
    }
    size_t const avail = free_size - margin;
    return (requested < avail) ? requested : avail;
}

#ifdef __cplusplus
}
#endif

#endif /* MC_TRANSPORT_UART_ACCEPT_H */
