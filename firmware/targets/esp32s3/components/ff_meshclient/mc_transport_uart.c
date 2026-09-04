/**
 * mc_transport_uart.c — see mc_transport_uart.h for the port-choice and
 * backpressure-contract reasoning.
 */
#include "mc_transport_uart.h"

#include "esp_log.h"

#include "mc_transport_uart_accept.h"

static const char *TAG = "mc_uart";

/* Reserve for uart_tx_all()'s own internal `uart_tx_data_t` header item
 * (a SEPARATE xRingbufferSend() call ahead of the payload bytes on every
 * uart_write_bytes() call — see esp_driver_uart/src/uart.c). 32 bytes is
 * generous headroom over that struct's actual size plus the ring's own
 * per-item alignment/header bytes; see mc_transport_uart_accept.h's top
 * comment for why this margin exists at all. */
#define MC_UART_WRITE_MARGIN 32u

esp_err_t mc_uart_open(mc_uart_t *t, mc_uart_cfg_t const *cfg)
{
    if (t == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    t->port = cfg->port;
    t->driver_installed = false;

    if (cfg->rx_ring_bytes < MC_UART_RX_RING_MIN || cfg->tx_ring_bytes < MC_UART_TX_RING_MIN) {
        ESP_LOGE(TAG, "ring sizes below spec minimum (rx=%u/%u tx=%u/%u)", (unsigned)cfg->rx_ring_bytes,
                 (unsigned)MC_UART_RX_RING_MIN, (unsigned)cfg->tx_ring_bytes, (unsigned)MC_UART_TX_RING_MIN);
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t const uart_cfg = {
        .baud_rate = cfg->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, /* Meshtastic's Serial module has no RTS/CTS */
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* TX/RX ring buffers both nonzero: the RX ring is required by
     * uart_driver_install() for any buffered (non-REF_TICK) use, and a
     * nonzero TX ring is what makes uart_write_bytes() a ring-buffered
     * call in the first place (see mc_transport_uart_accept.h's header
     * comment on why a zero TX ring's direct-FIFO path, uart_tx_all()'s
     * else branch, blocks on a semaphore instead — no better for us).
     * No event queue (queue_size=0, uart_queue=NULL): this transport
     * polls via uart_read_bytes(..., 0) every mc_tick(), it does not
     * need ISR event notifications. intr_alloc_flags=0: default flags. */
    esp_err_t err = uart_driver_install(cfg->port, (int)cfg->rx_ring_bytes, (int)cfg->tx_ring_bytes, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install(port=%d) failed: %s", (int)cfg->port, esp_err_to_name(err));
        return err;
    }
    t->driver_installed = true;

    err = uart_param_config(cfg->port, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config(port=%d) failed: %s", (int)cfg->port, esp_err_to_name(err));
        mc_uart_close(t);
        return err;
    }

    /* RTS/CTS left UART_PIN_NO_CHANGE — no flow control on this link. */
    err = uart_set_pin(cfg->port, cfg->txd_gpio, cfg->rxd_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin(port=%d, txd=%d, rxd=%d) failed: %s", (int)cfg->port, cfg->txd_gpio,
                 cfg->rxd_gpio, esp_err_to_name(err));
        mc_uart_close(t);
        return err;
    }

    ESP_LOGI(TAG, "UART%d up: TXD=GPIO%d RXD=GPIO%d %d 8N1, rx_ring=%uB tx_ring=%uB", (int)cfg->port,
             cfg->txd_gpio, cfg->rxd_gpio, cfg->baud_rate, (unsigned)cfg->rx_ring_bytes,
             (unsigned)cfg->tx_ring_bytes);
    return ESP_OK;
}

void mc_uart_close(mc_uart_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->driver_installed) {
        (void)uart_driver_delete(t->port);
        t->driver_installed = false;
    }
}

/* read() — nonblocking per mc_transport_t's contract: ticks_to_wait=0
 * means "return immediately with whatever is already in the RX ring".
 * uart_read_bytes() returns:
 *   >0  bytes actually copied out — the common case.
 *    0  ring empty right now — NOT an error (mc_client.c's mc_tick()
 *       treats this as "nothing available" and simply stops its read
 *       loop for this call, exactly as documented).
 *   -1  parameter error (bad port, driver not installed) — a genuine
 *       hard failure, escalated by mc_tick() to an immediate reconnect,
 *       same as mc_transport_tcp's read() on a dead fd.
 * This is a direct 1:1 mapping — no translation needed. */
static int mc_uart_read_cb(void *io, uint8_t *buf, size_t maxlen)
{
    mc_uart_t *t = (mc_uart_t *)io;
    if (t == NULL || !t->driver_installed) {
        return -1;
    }
    int const n = uart_read_bytes(t->port, buf, (uint32_t)maxlen, 0);
    return n;
}

/* write() — must honour the #170 backpressure contract (mc_client.h's
 * mc_transport_t doc comment): 0..len accepted, 0 = "try again later"
 * (not an error), negative = hard failure only. See this file's header
 * comment and mc_transport_uart_accept.h for why a bare
 * uart_write_bytes(t->port, buf, len) call would violate this — with a
 * TX ring enabled, ESP-IDF's uart_tx_all() blocks the calling task
 * (portMAX_DELAY on xRingbufferSend()) for whatever doesn't currently
 * fit, which would stall mc_tick()'s shared UI tick exactly when the
 * link is busiest.
 *
 * Fix: ask uart_get_tx_buffer_free_size() how much room actually exists
 * BEFORE calling uart_write_bytes(), and never request more than that
 * (minus MC_UART_WRITE_MARGIN for uart_tx_all()'s own internal header
 * item) — so the uart_write_bytes() call this function actually makes is
 * known, in advance, to fit without blocking. */
static int mc_uart_write_cb(void *io, uint8_t const *buf, size_t len)
{
    mc_uart_t *t = (mc_uart_t *)io;
    if (t == NULL || !t->driver_installed) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    size_t free_size = 0;
    esp_err_t const err = uart_get_tx_buffer_free_size(t->port, &free_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_get_tx_buffer_free_size(port=%d) failed: %s", (int)t->port, esp_err_to_name(err));
        return -1; /* driver-level error — hard failure, not backpressure */
    }

    size_t const accept = mc_uart_write_accept_len(free_size, MC_UART_WRITE_MARGIN, len);
    if (accept == 0) {
        return 0; /* TX ring full right now — "try again later", not an error */
    }

    int const written = uart_write_bytes(t->port, buf, accept);
    if (written < 0) {
        ESP_LOGE(TAG, "uart_write_bytes(port=%d) failed", (int)t->port);
        return -1;
    }
    /* written == (int)accept in the normal case (we sized the request to
     * what free_size already guaranteed fits); returning the driver's own
     * count rather than assuming so is the more honest of the two. */
    return written;
}

mc_transport_t mc_uart_transport(mc_uart_t *t)
{
    mc_transport_t transport;
    transport.write = mc_uart_write_cb;
    transport.read = mc_uart_read_cb;
    transport.io = t;
    return transport;
}
