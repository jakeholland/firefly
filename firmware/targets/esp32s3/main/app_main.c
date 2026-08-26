/**
 * app_main.c — ESP32-S3 target entry point (S15 slice a — IDF skeleton).
 *
 * Scope, per docs/specs/S15-esp32s3-target.md slice (a) and the S15a task
 * brief: prove core+festpack+meshclient+app link and boot for xtensa.
 * NO peripheral is touched — no display, no touch, no sensors, no UART.
 * The Waveshare screen board hasn't arrived and the Seeed comms boards
 * haven't shipped, so this target brings the shell up against three
 * stub/no-op HALs and sits on Radar's honest no-fix state. Slices b/c/d
 * replace each stub with the real hardware driver.
 *
 * Mirrors targets/sim/main.c's shell setup shape (ff_shell_cfg_t, static
 * storage, ff_shell_init/tick) with every I/O source swapped for a stub:
 *
 *   ff_clock    -> esp_timer_get_time() (monotonic, matches the sim's
 *                  SDL_GetTicks()-backed clock in spirit: milliseconds
 *                  since an arbitrary epoch, never negative).
 *   ff_store    -> a no-op stub (see below) — NOT the real NVS-backed
 *                  store S15's spec eventually wants; that lands with a
 *                  later slice once settings actually need to survive a
 *                  reboot. Until then `ff_shell_load` reads "not found"
 *                  for everything and runs on defaults, which is the
 *                  honest behavior for a store that persists nothing.
 *   mc_transport -> left zeroed. ff_shell.h documents a transport whose
 *                  `read`/`write` are both NULL as "no transport": init
 *                  skips `mc_connect` entirely and the link stays
 *                  FF_SHELL_LINK_NONE. That is a MORE honest no-op than a
 *                  transport that "connects" and then never delivers
 *                  bytes — there is no handshake to fake here. Real UART
 *                  (GPIO43/44 -> comms brain D6/D7) is slice c.
 *
 * No festpack is loaded this slice either (`cfg.pack` stays NULL) — see
 * the S15a PR body for why the Lost Lands embed was deferred rather than
 * done "if trivial" per the brief. `ff_route_init`'s default base face is
 * `FF_APP_FACE_RADAR` regardless of pack state (app/ff_route.c), so the
 * shell reaches Radar's no-fix view without one; that view IS this
 * slice's success criterion, not a placeholder on the way to it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ff_shell.h"

static const char *TAG = "firefly";

/* ---------------------------------------------------------------------
 * ff_clock_t — monotonic milliseconds over esp_timer_get_time()
 * ------------------------------------------------------------------- */
static uint32_t ff_esp_clock_now_ms(void *user)
{
    (void)user;
    /* esp_timer_get_time() returns microseconds since boot as an int64_t
     * that only wraps after ~292,000 years — truncating to uint32_t ms
     * wraps every ~49.7 days, same as any embedded tick counter.
     * ff_clock_t's contract (platform/include/ff_clock.h) is exactly
     * that: callers compare with subtraction, not '<', so this is safe. */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------------
 * ff_store_t — no-op stub (S15a). Every get() misses, every set()
 * silently discards. This is deliberately NOT the NVS-backed store the
 * full S15 spec eventually wants (see this file's top comment) — nothing
 * peristed here survives a reboot, and ff_shell_init's settings load
 * therefore always falls back to defaults, which is the honest outcome
 * for a store that keeps nothing. A later slice swaps this for real NVS
 * without touching ff_shell_cfg_t's shape at all.
 * ------------------------------------------------------------------- */
static int ff_stub_store_get(void *io, char const *key, void *buf, size_t n)
{
    (void)io;
    (void)key;
    (void)buf;
    (void)n;
    return -1; /* "not found", per ff_store_t's documented contract */
}

static int ff_stub_store_set(void *io, char const *key, void const *buf, size_t n)
{
    (void)io;
    (void)key;
    (void)buf;
    return (int)n; /* pretend success; bytes go nowhere — see the doc above */
}

static ff_shell_t s_shell;
static ff_clock_t s_clock;
static ff_store_t s_store;

void app_main(void)
{
    ESP_LOGI(TAG, "firefly esp32s3 target booting (S15 slice a: IDF skeleton, no peripherals touched)");

    s_clock.now_ms = ff_esp_clock_now_ms;
    s_clock.user = NULL;

    s_store.get = ff_stub_store_get;
    s_store.set = ff_stub_store_set;
    s_store.io = NULL;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &s_clock;
    cfg.store = &s_store;
    /* cfg.transport left zeroed (both write and read NULL) — "no
     * transport", see this file's top comment. */
    /* cfg.pack left NULL — no festpack this slice. */
    /* cfg.haptic left NULL — no haptics HAL this slice. */

    int rc = ff_shell_init(&s_shell, &cfg);
    if (rc != 0) {
        ESP_LOGE(TAG, "ff_shell_init failed (rc=%d) — halting", rc);
        return;
    }

    /* First tick always reports dirty (ff_shell.h's ff_shell_tick doc) —
     * pull the view after it so the logged face reflects the real
     * projection, not the pre-tick zeroed struct. */
    (void)ff_shell_tick(&s_shell, ff_esp_clock_now_ms(NULL));
    ff_app_state_t const *view = ff_shell_view(&s_shell);
    ESP_LOGI(TAG, "shell up: active_face=%d radar_mode=%d link=%d", (int)view->active_face,
             (int)view->radar.mode, (int)ff_shell_link(&s_shell));

    /* Pump at ~50 Hz, matching mc_client.h's guidance (ff_shell.h's
     * ff_shell_tick doc). With no transport attached mc_client never has
     * bytes to process, so every tick after the first is inert — this
     * loop exists to prove the shell keeps running under FreeRTOS, not to
     * do anything more yet. */
    while (true) {
        (void)ff_shell_tick(&s_shell, ff_esp_clock_now_ms(NULL));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
