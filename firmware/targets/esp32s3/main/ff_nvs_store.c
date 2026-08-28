/**
 * ff_nvs_store.c — see ff_nvs_store.h. The NVS-backed ff_store_t (S21 §4).
 */
#include "ff_nvs_store.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "ff_nvs";

/* One namespace for all of the puck's persisted state. ff_settings writes
 * under its own key (FF_SETTINGS_STORE_KEY = "ff.settings", 11 chars, within
 * NVS's 15-char key limit) — future persisted modules get their own keys in
 * the same namespace. */
#define FF_NVS_NAMESPACE "firefly"

/* ff_store_t.get — read the blob under `key` into `buf` (capacity `n`).
 * Returns the value length on success; a negative on absent/too-small/failed,
 * never partially filling `buf` (the ff_store_t contract). */
static int ff_nvs_store_get(void *io, char const *key, void *buf, size_t n)
{
    ff_nvs_store_t *st = (ff_nvs_store_t *)io;
    if (st == NULL || !st->open || key == NULL || buf == NULL) {
        return -1;
    }

    size_t len = n;
    esp_err_t err = nvs_get_blob(st->handle, key, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return -1; /* no value yet -> the loader applies defaults */
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        /* Stored blob is larger than `buf` — the store contract's "buffer too
         * small" case. nvs_get_blob does not write a partial value here. */
        return -1;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_blob(%s) failed: %s", key, esp_err_to_name(err));
        return -1;
    }
    return (int)len;
}

/* ff_store_t.set — write `n` bytes as the value for `key`, replacing any
 * prior value, then commit. Returns n on success, negative on failure. */
static int ff_nvs_store_set(void *io, char const *key, void const *buf, size_t n)
{
    ff_nvs_store_t *st = (ff_nvs_store_t *)io;
    if (st == NULL || !st->open || key == NULL || (buf == NULL && n > 0)) {
        return -1;
    }

    esp_err_t err = nvs_set_blob(st->handle, key, buf, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_blob(%s) failed: %s", key, esp_err_to_name(err));
        return -1;
    }
    /* Commit per write: settings are saved on change only (never every tick —
     * ff_shell.c's shell_setting_set gates on an actual value change), so
     * committing each save is a handful of writes per session, not a hot
     * path, and it guarantees the value is durable before we return. */
    err = nvs_commit(st->handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit(%s) failed: %s", key, esp_err_to_name(err));
        return -1;
    }
    return (int)n;
}

ff_store_t ff_nvs_store_init(ff_nvs_store_t *io)
{
    ff_store_t st;
    st.get = ff_nvs_store_get;
    st.set = ff_nvs_store_set;
    st.io = io;

    if (io == NULL) {
        return st;
    }
    io->handle = 0;
    io->open = false;

    /* Standard ESP-IDF idiom: on a partition with no free pages or a newer
     * on-flash format than this build understands, erase and retry once. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s — erasing NVS partition and retrying", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s — settings will not persist this boot (defaults, no save)",
                 esp_err_to_name(err));
        return st; /* io->open stays false -> get/set degrade to no-op */
    }

    err = nvs_open(FF_NVS_NAMESPACE, NVS_READWRITE, &io->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s — settings will not persist this boot", FF_NVS_NAMESPACE,
                 esp_err_to_name(err));
        return st; /* io->open stays false */
    }

    io->open = true;
    ESP_LOGI(TAG, "NVS store ready (namespace '%s') — settings persist across reboot", FF_NVS_NAMESPACE);
    return st;
}
