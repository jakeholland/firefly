/**
 * ff_t9dict.h — the compiled predictive-T9 dictionary data (rodata / flash).
 *
 * The table itself is GENERATED: `core/tools/gen_t9dict.py` emits
 * `core/src/ff_t9dict_data.c` from the committed `core/tools/t9dict_words.txt`
 * frequency list. Source + license: `core/tools/NOTICE-t9dict.md`.
 *
 * The dictionary is a single NUL-terminated ASCII blob of lowercase `[a-z]`
 * words, laid out **most-frequent-first** (index 0 = most common). Because the
 * blob is already in frequency order, the engine (`ff_t9pred`) can walk it
 * sequentially and the first prefix-matches it finds are, by construction, the
 * most frequent — no offset table, no per-query sort, no heap.
 *
 * These symbols are `const` (rodata → flash, which has room; never IRAM) and
 * carry no behavior; all logic lives in `ff_t9pred.c`. They are exposed in a
 * header only so the engine and its tests can see the same declarations.
 */
#ifndef FF_T9DICT_H
#define FF_T9DICT_H

#ifdef __cplusplus
extern "C" {
#endif

/** Concatenated words, each NUL-terminated, in descending frequency order.
 *  e.g. "the\0and\0of\0...". Also NUL-terminated as a whole (the final word's
 *  terminator), so a plain forward walk of `ff_t9dict_count` words is safe. */
extern const char ff_t9dict_blob[];

/** Number of words in the blob. */
extern const unsigned ff_t9dict_count;

/** Total bytes in `ff_t9dict_blob` including every terminator. */
extern const unsigned ff_t9dict_blob_len;

#ifdef __cplusplus
}
#endif

#endif /* FF_T9DICT_H */
