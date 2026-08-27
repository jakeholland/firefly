#!/usr/bin/env python3
"""gen_t9dict.py — build the predictive-T9 dictionary table for core/t9pred.

Two reproducible stages, both committed so the whole chain can be re-run:

  1. harvest  — download a pinned set of PUBLIC-DOMAIN Project Gutenberg texts,
                tokenise, count, and write a frequency-ranked wordlist to
                `t9dict_words.txt` (`word<TAB>count`, one per line, most
                frequent first, ties broken alphabetically for determinism).
                Requires network; run only when regenerating the wordlist.

  2. emit     — read `t9dict_words.txt` (committed) and generate the C source
                `../src/ff_t9dict_data.c`: a NUL-terminated, frequency-ordered
                word blob placed in rodata (flash), no heap, no I/O. This is
                the step the build depends on; it needs no network.

Usage:
    python3 gen_t9dict.py harvest [t9dict_words.txt]
    python3 gen_t9dict.py emit    [t9dict_words.txt] [../src/ff_t9dict_data.c]

## Dictionary source + license (see also core/tools/NOTICE-t9dict.md)

The wordlist is a frequency ranking derived from public-domain works
distributed by Project Gutenberg (US works whose copyright has expired — the
texts themselves carry no usage restrictions). A frequency ranking of
public-domain text is itself free to redistribute, which is why this source
was chosen over popular lists such as `google-10000-english`: that list's own
LICENSE states its data derives from the Google Trillion Word Corpus under the
Linguistic Data Consortium license and is NOT recommended for commercial use —
incompatible with vendoring into GPL-3.0 Firefly. Project Gutenberg PD text
has no such encumbrance.

The pinned book IDs below span 19th/early-20th-century fiction, adventure,
science fiction and philosophy to reduce single-author vocabulary skew. Being
a pre-modern literary corpus, it honestly under-represents modern colloquial
vocabulary (e.g. "hello", "okay", "phone" do not make the top slice); the
engine returns an explicit no-match for such sequences rather than inventing a
word (see ff_t9pred.h).
"""
import collections
import re
import sys
import urllib.request

# Pinned Project Gutenberg book IDs (public domain). Changing this set changes
# the wordlist — keep it pinned so `harvest` is reproducible.
BOOKS = [
    1342, 84, 1661, 2701, 98, 1400, 74, 76, 345, 174, 2591, 11, 1232,
    25344, 46, 16, 35, 36, 158, 219, 205, 768, 1080, 5200, 1260, 120,
    64317, 2542, 1497, 4517, 2814, 3207, 2680, 244, 100, 863, 1727,
]

TOP_N = 3200          # size of the compiled dictionary
MAX_WORD_LEN = 24     # must equal FF_T9PRED_MAX_DIGITS in ff_t9pred.h


def fetch(bid):
    urls = (
        f"https://www.gutenberg.org/files/{bid}/{bid}-0.txt",
        f"https://www.gutenberg.org/files/{bid}/{bid}.txt",
        f"https://www.gutenberg.org/cache/epub/{bid}/pg{bid}.txt",
    )
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                return r.read().decode("utf-8", "ignore")
        except Exception:
            continue
    print(f"  !! failed to fetch {bid}", file=sys.stderr)
    return ""


def strip_gutenberg(text):
    """Trim Project Gutenberg header/footer boilerplate."""
    start = re.search(r"\*\*\* START OF.*?\*\*\*", text, re.S)
    end = re.search(r"\*\*\* END OF", text, re.S)
    if start:
        text = text[start.end():]
    if end:
        text = text[:end.start()]
    return text


def harvest(out_path):
    counts = collections.Counter()
    for bid in BOOKS:
        print(f"fetch {bid}", file=sys.stderr)
        text = strip_gutenberg(fetch(bid))
        for w in re.findall(r"[a-z]+", text.lower()):
            # Single letters are noise except the real words "a" and "i".
            if len(w) == 1 and w not in ("a", "i"):
                continue
            if len(w) > MAX_WORD_LEN:
                continue
            counts[w] += 1
    ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    with open(out_path, "w") as f:
        for word, count in ranked[:TOP_N]:
            f.write(f"{word}\t{count}\n")
    print(f"unique={len(counts)} written={min(TOP_N, len(ranked))} -> {out_path}",
          file=sys.stderr)


def read_wordlist(path):
    words = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            word = line.split("\t", 1)[0]
            if not re.fullmatch(r"[a-z]+", word):
                raise SystemExit(f"non [a-z] word in {path!r}: {word!r}")
            if len(word) > MAX_WORD_LEN:
                raise SystemExit(f"word longer than MAX_WORD_LEN in {path!r}: {word!r}")
            words.append(word)
    # De-dup defensively while preserving (frequency) order.
    seen = set()
    uniq = []
    for w in words:
        if w not in seen:
            seen.add(w)
            uniq.append(w)
    return uniq


def emit(words_path, out_path):
    words = read_wordlist(words_path)
    blob = bytearray()
    for w in words:
        blob += w.encode("ascii")
        blob += b"\0"
    if len(blob) > 0xFFFF:
        raise SystemExit(f"blob {len(blob)} bytes exceeds uint16 offset space")

    lines = []
    lines.append("/* GENERATED by core/tools/gen_t9dict.py — DO NOT EDIT BY HAND.")
    lines.append(" *")
    lines.append(" * Predictive-T9 dictionary: a frequency-ranked English wordlist")
    lines.append(" * derived from public-domain Project Gutenberg texts. Source, license")
    lines.append(" * and reproduction steps: core/tools/gen_t9dict.py + NOTICE-t9dict.md.")
    lines.append(" *")
    lines.append(f" * {len(words)} words, most-frequent-first (ties broken alphabetically).")
    lines.append(" * Stored as one NUL-terminated ASCII blob in rodata (flash), so the")
    lines.append(" * engine can walk it in rank order with no heap and no offset table.")
    lines.append(" */")
    lines.append('#include "ff_t9dict.h"')
    lines.append("")
    lines.append("const unsigned ff_t9dict_count = %u;" % len(words))
    lines.append("const unsigned ff_t9dict_blob_len = %u;" % len(blob))
    lines.append("")
    lines.append("const char ff_t9dict_blob[] =")

    # Emit the blob as C string literals, one word (with its \0) per line, in
    # readable chunks. Using per-word literals keeps the generated file diff-
    # friendly and lets a reader eyeball the ranking.
    chunk = []
    col = 0
    for w in words:
        lit = '"%s\\0"' % w
        chunk.append(lit)
        col += len(lit)
        if col >= 72:
            lines.append("    " + "".join(chunk))
            chunk = []
            col = 0
    if chunk:
        lines.append("    " + "".join(chunk))
    lines[-1] = lines[-1] + ";"
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print(f"emit {len(words)} words, blob={len(blob)} bytes -> {out_path}",
          file=sys.stderr)


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "emit"
    if cmd == "harvest":
        out = argv[2] if len(argv) > 2 else "t9dict_words.txt"
        harvest(out)
    elif cmd == "emit":
        words_path = argv[2] if len(argv) > 2 else "t9dict_words.txt"
        out_path = argv[3] if len(argv) > 3 else "../src/ff_t9dict_data.c"
        emit(words_path, out_path)
    else:
        raise SystemExit(f"unknown command {cmd!r}; use 'harvest' or 'emit'")


if __name__ == "__main__":
    main(sys.argv)
