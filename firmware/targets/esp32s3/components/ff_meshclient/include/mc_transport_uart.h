/**
 * mc_transport_uart.h — ESP-IDF UART transport for mc_transport_t
 * (S15c, docs/specs/S15-esp32s3-target.md slice c).
 *
 * The device-side counterpart to firmware/meshclient/mc_transport_tcp.h
 * (the sim's dev stand-in): wires this device to the comms brain (a
 * Seeed XIAO ESP32S3 + Wio-SX1262 running stock Meshtastic, Serial
 * module in PROTO mode) over a UART header exposed on the Waveshare
 * ESP32-S3-Touch-LCD-1.46: TXD=GPIO43, RXD=GPIO44, 3V3/5V/GND — all
 * Kconfig-selected (FF_UART_TXD/FF_UART_RXD/FF_UART_BAUD/FF_UART_PORT,
 * see targets/esp32s3/main/Kconfig.projbuild), so a header remap needs
 * no code change.
 *
 * Port choice (default UART1, Kconfig FF_UART_PORT): ESP32-S3's UART
 * peripherals route through the GPIO matrix, so either UART0 or UART1
 * CAN be wired to GPIO43/44 via uart_set_pin() — the choice is about
 * avoiding a conflict elsewhere, not about wiring capability.
 *
 *   - UART0 is NOT free by default. GPIO43/44 are ALSO UART0's own
 *     default pins, and this repo's committed sdkconfig.defaults leaves
 *     ESP-IDF's stock console config in place: PRIMARY console = UART0,
 *     SECONDARY console = USB-Serial/JTAG (confirmed against
 *     esp-idf/components/esp_system/Kconfig's ESP_CONSOLE_UART/
 *     ESP_CONSOLE_SECONDARY choices — the default combo, not a project
 *     override — and against this file's own app_main.c, whose S26f
 *     comment states outright "USB-Serial/JTAG is the SECONDARY
 *     console" on the maintainer's real board). esp_console already owns
 *     a uart_driver_install() on UART0 in that configuration; a second
 *     install here would either fail (ESP_ERR_INVALID_STATE) or, if it
 *     somehow didn't, interleave this transport's framed bytes with
 *     plain-text log lines on the same wire. Using UART0 for the mesh
 *     link is only safe once a maintainer deliberately reconfigures the
 *     console to USB-Serial/JTAG ONLY (dropping UART0 from console duty
 *     entirely) — a system-wide sdkconfig change this slice does not
 *     make. FF_UART_PORT_0 exists for that future, verified case.
 *   - UART1 has no such conflict: nothing else in this build touches it.
 *     Remapping it onto GPIO43/44 costs nothing and needs no console
 *     change, so it is the default.
 *
 * write() honours the #170 backpressure contract stated in
 * mc_client.h's mc_transport_t doc comment (0..len bytes accepted, 0 =
 * "try again later", negative = hard failure only) — see
 * mc_transport_uart_accept.h for why a naive uart_write_bytes() call
 * would violate it (it can block the calling task indefinitely against
 * a full TX ring) and how this transport avoids that.
 */
#ifndef MC_TRANSPORT_UART_H
#define MC_TRANSPORT_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#include "mc_client.h" /* mc_transport_t */

#ifdef __cplusplus
extern "C" {
#endif

/* RX ring: >= 2 KB per spec — a want_config NodeInfo burst can be large
 * (mc_client.h's MC_TICK_MAX_FRAMES doc: "a busy public channel can carry
 * on the order of 100 nodes"), and this ring is drained only once per
 * mc_tick() call (~50 Hz), so it must absorb whatever arrives between
 * ticks without dropping bytes. */
#define MC_UART_RX_RING_MIN 2048u
/* TX ring: >= 1 KB per spec — comfortably larger than one framed
 * ToRadio message (MC_MAX_FRAME + 4, mc_framing.h), so an ordinary send
 * completes in one write() call under normal drain rates; back-pressure
 * past that is exactly what the 0-return contract exists for. */
#define MC_UART_TX_RING_MIN 1024u

/**
 * mc_uart_t — the transport's `io` object. One per UART link; must
 * outlive the mc_transport_t handed to mc_init()/ff_shell_cfg_t, same
 * contract as mc_tcp_t.
 */
typedef struct {
    uart_port_t port;
    bool driver_installed; /* mc_uart_close() only tears down what mc_uart_open() built */
} mc_uart_t;

/** Configuration for mc_uart_open() — every field is Kconfig-sourced in
 *  app_main.c (CONFIG_FF_UART_PORT/FF_UART_TXD/FF_UART_RXD/FF_UART_BAUD),
 *  never hardcoded here, so a header remap is a menuconfig change, not a
 *  code change. */
typedef struct {
    uart_port_t port;
    int txd_gpio;
    int rxd_gpio;
    int baud_rate;
    size_t rx_ring_bytes; /* must be >= MC_UART_RX_RING_MIN */
    size_t tx_ring_bytes; /* must be >= MC_UART_TX_RING_MIN */
} mc_uart_cfg_t;

/**
 * mc_uart_open — install the UART driver at 8N1 + the given baud, on the
 * given port, with TX/RX pins remapped via uart_set_pin() (RTS/CTS left
 * UART_PIN_NO_CHANGE — this link has no flow control, matching
 * Meshtastic's Serial module).
 *
 * Returns ESP_OK on success; `*t` is valid either way (mc_uart_close() is
 * always safe to call). On failure, `*t` reports no driver installed and
 * the caller should treat the link as absent (same "no transport"
 * fallback ff_shell_cfg_t.transport documents for a zeroed vtable) rather
 * than retry here — app_main.c logs the esp_err_t and moves on, matching
 * this file's "log and continue" bring-up convention.
 */
esp_err_t mc_uart_open(mc_uart_t *t, mc_uart_cfg_t const *cfg);

/** mc_uart_close — uninstalls the UART driver if mc_uart_open() actually
 *  installed one. Safe on a zero-initialised or never-opened *t. */
void mc_uart_close(mc_uart_t *t);

/** mc_uart_transport — the mc_transport_t vtable bound to `t`. `t` must
 *  already be open (or intentionally left unopened for the "no driver"
 *  case, in which case read()/write() report -1 — see the .c file). */
mc_transport_t mc_uart_transport(mc_uart_t *t);

#ifdef __cplusplus
}
#endif

#endif /* MC_TRANSPORT_UART_H */
