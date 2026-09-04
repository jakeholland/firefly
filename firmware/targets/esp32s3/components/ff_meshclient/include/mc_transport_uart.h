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
 * A review round on this slice corrected an earlier version of this
 * comment that claimed "esp_console already owns a uart_driver_install()
 * on UART0" — checked against esp-idf/components/esp_driver_uart/src/
 * uart_vfs.c and that is NOT what happens: the console's normal ESP_LOGx/
 * stdout path writes via uart_tx_char(), a direct busy-wait poke on the
 * raw UART TX FIFO register (uart_ll_get_txfifo_len()) — it never calls
 * uart_driver_install() unless the app opts in via
 * uart_vfs_dev_use_driver() (this app doesn't). The only esp-idf caller of
 * uart_driver_install() for anything console-shaped is
 * components/console/esp_console_repl_chip.c — the interactive REPL
 * component, unused here. So a second uart_driver_install() on UART0
 * would not fail at install time the way the earlier comment claimed.
 *
 * The real hazard, and why this repo's sdkconfig.defaults/sdkconfig.ci
 * now set the console to USB-Serial/JTAG ONLY (S15 Amendments,
 * docs/specs/S15-esp32s3-target.md) rather than leaving ESP-IDF's stock
 * default (PRIMARY console = UART0, SECONDARY = USB-Serial/JTAG — the
 * default combo, esp-idf/components/esp_system/Kconfig's ESP_CONSOLE_UART/
 * ESP_CONSOLE_SECONDARY choices) in place: TWO INDEPENDENT PRODUCERS
 * writing raw bytes to the SAME physical UART0 TX FIFO register with no
 * shared coordination. esp_log's uart_tx_char() can fire from any task or
 * ISR context at any time a log line is emitted; this transport's own
 * uart_driver_install()'d engine ALSO drives that identical hardware FIFO
 * from its own interrupt handler. Neither knows the other exists — a log
 * line and a mesh frame's bytes can interleave or corrupt each other on
 * the wire. That is shared-FIFO corruption, not an install-time API
 * conflict.
 *
 * UART1 sidesteps that hazard entirely (nothing else in this build ever
 * touches it) AND is the port this slice keeps even now that the console
 * has moved to USB-Serial/JTAG only: moving the CONSOLE off UART0 does
 * not fully silence GPIO43/44 at every point in boot. The ROM's own
 * first-stage boot banner ("ets ... rst:0x1 (POWERON)...") is
 * hardware-fixed to UART0's default pins and prints before any flash
 * code — bootloader or app — has read a single Kconfig byte, so it cannot
 * be moved by any sdkconfig setting. See the S15 Amendment's
 * "boot-garbage window" entry for the full reasoning (bounded, one-time
 * per boot, absorbed by whichever side's framer resync counter sees it —
 * this device's mc_get_stats().frames_resynced if it arrives on our RX,
 * the comms brain's own Meshtastic framer if it goes out our TX). Putting
 * the mesh link on UART1 means this transport's own operation is
 * completely unaffected by that ROM-level behavior either way.
 *
 * UART0 stays offered via FF_UART_PORT_0 for a future maintainer who has
 * independently verified both of the above no longer apply to their
 * build — this slice does not attempt either fix.
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

/* RX ring: 8 KB — well above the >=2 KB spec floor (S15c review round:
 * a want_config NodeInfo burst can be large — mc_client.h's
 * MC_TICK_MAX_FRAMES doc: "a busy public channel can carry on the order
 * of 100 nodes" — and this ring is drained only once per mc_tick() call
 * (~50 Hz), so it must absorb whatever arrives between ticks without
 * dropping bytes). Raised from the spec-floor 2 KB to 8 KB: DIRAM headroom
 * on this build is ample (idf.py size on the CONFIG_FF_LINK_UART build:
 * ~159 KB DIRAM remaining out of 334 KB total, see the S15 Amendment's
 * gate table), so there is no reason to run the want_config burst case
 * close to the 2 KB floor when 8 KB costs a few percent of a resource
 * this build isn't remotely short on — cheap insurance against a larger
 * real-world burst than the 100-node estimate above. */
#define MC_UART_RX_RING_MIN 8192u
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
