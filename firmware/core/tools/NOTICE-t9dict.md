# Predictive-T9 dictionary — source & license notice

The compiled dictionary in `firmware/core/src/ff_t9dict_data.c` (used by
`core/t9pred`, the predictive text-entry engine) is a **frequency-ranked
English wordlist derived from public-domain Project Gutenberg texts**.

## Source

- **Corpus:** a pinned set of Project Gutenberg books (the `BOOKS` list in
  `gen_t9dict.py`) — 19th/early-20th-century public-domain fiction, adventure,
  science fiction, and philosophy, chosen to spread vocabulary across authors.
- **Method:** each text is stripped of Project Gutenberg header/footer
  boilerplate, lowercased, tokenised to `[a-z]+` runs, and counted. Words are
  ranked most-frequent-first (ties broken alphabetically for determinism); the
  top 3200 are compiled in. Fully reproducible: `python3 gen_t9dict.py harvest`
  rebuilds `t9dict_words.txt`, then `python3 gen_t9dict.py emit` regenerates
  the C table.

## License

Project Gutenberg texts used here are in the **public domain in the United
States** (their copyright has expired); Project Gutenberg applies no usage
restrictions to public-domain content, and a **frequency ranking of
public-domain text is itself free to redistribute**. The wordlist and the
generated table are therefore compatible with Firefly's GPL-3.0 license and
carry no additional obligations.

## Why not `google-10000-english` / Norvig `count_1w`?

Those popular frequency lists were deliberately **not** used. Their own license
(`google-10000-english/LICENSE.md`) states the data derives from the *Google
Trillion Word Corpus* distributed under the **Linguistic Data Consortium**
license, that only "educational and personal/research use" is permitted, and
that commercial use is not recommended without an LDC license. That is
incompatible with vendoring into a GPL-3.0, trademark-protected project.
Public-domain Gutenberg text has no such encumbrance.

## Honest limitation

Being a pre-modern literary corpus, the list under-represents modern colloquial
words — e.g. **"hello", "okay", "phone", "beer"** do not appear in the top
slice. For those digit sequences the engine returns an **explicit no-match**
(it never fabricates a word); the composer falls back to multi-tap. Swapping in
a modern public-domain / permissively-licensed frequency corpus later requires
only re-running `gen_t9dict.py` — no engine change.
