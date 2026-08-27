# Predictive-T9 dictionary — source & license notice

The compiled dictionary in `firmware/core/src/ff_t9dict_data.c` (used by
`core/t9pred`, the predictive text-entry engine) is a **merge of two ranked
layers**: a **hand-curated, project-authored festival/texting vocabulary**
ranked first, followed by a **frequency-ranked English tail derived from
public-domain Project Gutenberg texts**.

## Layer 1 — curated festival vocabulary (high rank)

- **Source:** `firmware/core/tools/t9dict-curated.txt` — a hand-authored list
  of modern festival-messenger vocabulary: greetings, texting abbreviations,
  reactions, raver/EDM/festival slang, and coordination/logistics terms (the
  words a pre-modern literary corpus cannot cover — "hello", "okay", "tbh",
  "wtf", "lol", "rave", "kandi", "camp"). One token per line, hand-ranked
  roughly most-common-first; `#` comments and blank lines are ignored.
- **Method:** `gen_t9dict.py emit` folds this file in FIRST (above the
  Gutenberg tail), lowercased, de-duplicated in file order. Tokens that are not
  typeable on a letter-only T9 keypad (anything outside `[a-z]`, e.g. "b2b")
  are skipped, since the engine maps only letter keys 2..9. A curated word that
  also appears in the Gutenberg tail keeps its higher curated rank and is
  dropped from the tail.
- **License:** authored by the **Firefly project (GPL-3.0)**. It is plain,
  common words plus public slang — **no third-party corpus, no licensing
  encumbrance**. Editorial choices (documented in the file header): alcohol is
  normal festival vocabulary and included; hard-drug slang is deliberately kept
  OUT of the shipped default for brand/trademark safety; apostrophes are
  dropped to match keypad entry ("dont", "im").

## Layer 2 — Gutenberg frequency tail (general-English long tail)

- **Corpus:** a pinned set of Project Gutenberg books (the `BOOKS` list in
  `gen_t9dict.py`) — 19th/early-20th-century public-domain fiction, adventure,
  science fiction, and philosophy, chosen to spread vocabulary across authors.
- **Method:** each text is stripped of Project Gutenberg header/footer
  boilerplate, lowercased, tokenised to `[a-z]+` runs, and counted. Words are
  ranked most-frequent-first (ties broken alphabetically for determinism); the
  top 3200 form the tail slice (minus any word already in the curated layer).
  Fully reproducible: `python3 gen_t9dict.py harvest` rebuilds
  `t9dict_words.txt`, then `python3 gen_t9dict.py emit` regenerates the C
  table by merging both layers.

## License

Project Gutenberg texts used here are in the **public domain in the United
States** (their copyright has expired); Project Gutenberg applies no usage
restrictions to public-domain content, and a **frequency ranking of
public-domain text is itself free to redistribute**. The curated layer is
original project work under GPL-3.0. Both the wordlists and the generated table
are therefore compatible with Firefly's GPL-3.0 license and carry no additional
obligations.

## Why not `google-10000-english` / Norvig `count_1w`?

Those popular frequency lists were deliberately **not** used. Their own license
(`google-10000-english/LICENSE.md`) states the data derives from the *Google
Trillion Word Corpus* distributed under the **Linguistic Data Consortium**
license, that only "educational and personal/research use" is permitted, and
that commercial use is not recommended without an LDC license. That is
incompatible with vendoring into a GPL-3.0, trademark-protected project.
Public-domain Gutenberg text has no such encumbrance.

## Modern-vocabulary coverage

The pre-modern Gutenberg corpus honestly under-represents modern colloquial
words — historically **"hello", "okay", "phone", "beer"** did not appear in the
top slice, and those digit sequences returned an explicit no-match. The curated
layer (layer 1) closes that gap: those words now resolve to themselves as a top
candidate. The engine still returns an **honest no-match** for any sequence no
word in either layer covers (it never fabricates a word — see ff_t9pred.h); the
composer falls back to multi-tap there.
