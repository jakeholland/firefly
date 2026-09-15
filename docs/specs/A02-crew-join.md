# A02 · crew join — Start a crew / Join a crew, auto membership, plain-language crew UX

> **A-series.** A01 built the client. This spec is about the one thing
> that client still cannot do: get eight friends onto the same crew in a
> field, in under five minutes, without any of them learning the word
> "channel". Same rules as every other spec — acceptance criteria become
> test names (`A02_AC3_…`), unknowns are represented rather than papered
> over, and anything cut is cut out loud.
>
> Firmware counterpart: `docs/specs/S02-core-crew.md`'s **2026-09-13
> amendment — auto-crew on the crew channel**, which carries the puck
> half of this design (auto membership, the hide list, the code face,
> `FF_CREW_AUTO_ON_CHANNEL`). This file owns the app; that amendment
> owns the puck; the codec in section 1 is shared by both, byte for
> byte.

## Motivation

Two independent UX reviews walked the shipped app as a first-time
festival user (Maya, 27, never heard of Meshtastic; Deshawn, 34,
organising eight people at Lost Lands). Both reviews were **session
artifacts under `/private/tmp`, not repo files** — they are gone with
the next reboot, so everything either one is relied on for is
reproduced below rather than cited to a path. They independently
reached the same two conclusions:

1. **There is no way to create or share a crew from inside this app.**
   Connect's CHANNEL card only *imports* a link. Per
   `docs/hardware/comms-brain.md`, the crew channel has to be minted
   upstream — in a different app, or on the CLI — before Firefly ever
   enters the picture. That is not a rough edge, it is a hole where the
   product's first step should be.
2. **Being on the same channel still does not make anyone visible.**
   The roster-trust policy (S16, `ff_shell.h`'s "THE ROSTER TRUST
   POLICY") refuses to grow the crew from anything the radio says, so
   every person must manually ADD every other person. Deshawn's arithmetic:
   **7 taps × 8 phones = 56 manual adds**, each gated on that specific
   phone having already heard that specific radio, with no shared view
   of who is done.

Both reviews then independently drew the same fix, which is also the
owner's decision of 2026-09-13: **anyone who has the crew code is
crew.** The firmware already proves this works — it is exactly what
`CONFIG_FF_DEV_TRUST_CHANNEL` does — it has just never been anything
but a debug flag with no UI and a Kconfig help text that says it must
never ship on.

This spec promotes that behaviour to the product, and puts a crew code
in front of it that a human can show, scan, or read out loud across a
tent.

## Owner decisions (Jake, 2026-09-13) — not up for re-litigation here

| Decision | Consequence in this spec |
|---|---|
| Anyone who has the crew code **is** crew, automatically. No manual add. | §4 auto-membership; the ADD action is deleted, not moved |
| Per-person **hide**, for stragglers and strangers you don't want on your radar | §4 hide list; hide is also how the 8-slot cap is managed |
| No "channel", "node", "region", "preset" or "Meshtastic" on the main path — Advanced only | §6 copy; §6.5 Advanced inventory |
| FIND keeps running when switching Find segments | §6.7 — a correction to A01's current segment lifecycle |
| The puck must behave the same way (auto-crew on the crew channel) | S02 amendment; `FF_CREW_AUTO_ON_CHANNEL=y` by default |

## What this replaces

A01 "Decisions from the owner" #5 said: **"Channel PSK handling: the app
never mints one."** That decision is **reversed here, deliberately and
in full.** Its own text anticipated this: *"If that changes later it is
a deliberate scope expansion, not an oversight to paper over here."*
This is that scope expansion. The reasoning it was based on — that
provisioning is CLI territory — was written when the app had no
write-back path at all; PR #274 has since landed `applyChannelSet`,
the plan/confirm/read-back machinery, and the admin-write confirmation
sheet, so minting a key is now the *small* half of the job. See §1.6
for the honest threat model that comes with minting keys from a
six-character code.

---

## 1. The crew code, and the key it derives

### 1.1 Shape

```
FIRE-4K9M7X
└──┘ └────┘
 tag  6 symbols, Crockford base32, 30 bits of entropy
```

- **Alphabet** (32 symbols, Crockford base32):
  `0123456789ABCDEFGHJKMNPQRSTVWXYZ` — no `I`, no `L`, no `O`, no `U`.
  `I`/`L` are excluded because they are unreadable against `1` in the
  app's mono face at arm's length in the dark; `O` against `0`; `U`
  because Crockford excludes it to keep accidental obscenities out of
  generated codes, and matching the published alphabet exactly is worth
  more than the one extra symbol.
- **Canonical form** is exactly 11 ASCII characters: `FIRE-` + six
  symbols, uppercase. This length is load-bearing — see §1.3.
- **Entropy**: 6 × 5 = **30 bits**, drawn from a CSPRNG
  (`SecRandomCopyBytes` on Apple platforms, `esp_random`/
  `esp_fill_random` on the puck), encoded MSB-first from the 30-bit
  integer. Never from a name, a timestamp, a node id, or anything else
  guessable.

> The approved artboards render the sample code as `FIRE-4L9K`. That
> sample is illustrative and **wrong on two counts** against this spec:
> it is four symbols, not six, and `L` is not in the alphabet. Every
> piece of shipped copy uses six valid symbols; the artboard sample
> becomes `FIRE-4K9M7X`, which is also this spec's test vector 1.

### 1.2 Normalising what a human types

`CrewCode.parse(_:)` accepts anything and either returns the canonical
form or fails. In order:

1. Trim whitespace; uppercase.
2. Strip all spaces and `-`.
3. Strip a leading `FIRE` if present (so `FIRE-4K9M7X`, `fire 4k9m7x`
   and `4k9m7x` are the same code). This runs **before** step 4, so the
   tag is matched literally. Consequence, pinned rather than
   discovered: a crew whose six symbols are `F1RE9X` parses correctly
   from the full spelling (`FIRE-FIRE9X` → strip tag → `FIRE9X` →
   alias → `F1RE9X`), but the tagless spelling `FIRE9X` is rejected
   rather than guessed at. Six-box entry (§3.2) prints `FIRE-` as a
   fixed prefix, so this only ever reaches pasted text.
4. Apply Crockford's decoding aliases: `I` → `1`, `L` → `1`, `O` → `0`.
   `U` is **not** aliased — it is rejected, so a typo lands on an error
   rather than silently on somebody else's crew.
5. Require exactly 6 remaining characters, all in the alphabet.
6. Re-render as `FIRE-` + the six symbols.

There is **no checksum**. A mistyped code is not silently wrong — it
derives a different PSK, so nothing decrypts and the Join screen's own
"nobody heard yet" state is the (honest) feedback. A checksum would cost
a symbol of entropy to catch a class of error the product already
surfaces within seconds.

### 1.3 Why the channel name *is* the code

Meshtastic does not put a channel index on the air. A `MeshPacket`
carries a one-byte **channel hash**, and the firmware computes that hash
as an XOR-fold of the channel *name* bytes XORed with an XOR-fold of the
*PSK* bytes — `xorHash(name) ^ xorHash(psk)`, `Channels::generateHash`
in meshtastic/firmware, which is **not in this checkout** (§1.9's bench
task is what actually confirms it); the receiving radio uses the byte to
pick which channel to try decrypting with.
`meshtastic/channel.proto`'s own comment makes the consequence explicit:
two people who agree on a name but not on a key "can't talk", and the
symmetric case is just as true — two people who agree on a key but not
on the **name** produce different hashes and never attempt each other's
packets.

So a free-text crew name cannot be the channel name: Deshawn typing
"Camp Firefly" and Maya joining by typed code alone (with no name to
type) would derive the same PSK and still be invisible to each other.

**Decision: the Meshtastic channel name is the canonical crew code,
verbatim.** `ChannelSettings.name` allows 11 usable bytes ("Less than 12
bytes", channel.proto) and `FIRE-` + 6 is exactly 11. This buys three
things beyond correctness:

- a typed code is sufficient to join, with nothing else needed;
- the puck can **derive the code it displays from its own channel
  name** — no extra NVS state, no second source of truth;
- anyone who opens the stock Meshtastic app on a crew radio sees
  `FIRE-4K9M7X` and can read the code straight off it.

The human name ("Camp Firefly") is **app-local display only**. It rides
in the deep link so a scanning joiner gets it, it is stored on the
phone, it is never written to the radio, and it is never part of any
derivation. A joiner who typed the code with no link sees the code as
the crew's name until they rename it locally (Crew page → tap the
name).

> Consequence, flagged not hidden: two people in the same crew can hold
> different human names for it. That is cosmetic and local, and the code
> underneath — shown on the Crew page under the name — is always the
> same. Broadcasting the crew name over the mesh so it converges is a
> real option and explicitly **out of scope**; it needs a new
> `ff_proto` message and is not worth it before the field test.

### 1.4 Key derivation

```
PSK = HKDF-SHA256(
        salt = "firefly-crew-v1"       (15 ASCII bytes, no NUL)
        ikm  = canonical code          (11 ASCII bytes, e.g. "FIRE-4K9M7X")
        info = "firefly-crew-psk-v1"   (19 ASCII bytes, no NUL)
        L    = 32)                     -> 32 bytes, AES256
```

RFC 5869 HKDF: `PRK = HMAC-SHA256(salt, ikm)`, then one-block-at-a-time
expand. 32 bytes needs exactly one expand block.

**`info` is a constant, NOT the crew name.** The prompt for this spec
floated `info = crew name`, and it must not be: the crew name is
optional (§1.3), so binding it into the key makes a typed-code join
underivable. The `info` string exists to domain-separate this key from
any future key derived from the same code (e.g. a signing key would use
`info = "firefly-crew-sign-v1"`), which is what `info` is for.

**`salt` is a constant, not a random salt.** HKDF's salt is allowed to
be a non-secret constant; here it must be, because both sides derive
independently from nothing but the code. It is version-tagged so a
future format change (`firefly-crew-v2`) cannot collide with a v1 code.

### 1.5 The channel that gets written

| Field | Value | Why |
|---|---|---|
| `ChannelSettings.psk` | the 32 derived bytes | AES256 |
| `ChannelSettings.name` | the canonical code, 11 chars | §1.3 |
| `ChannelSettings.channel_num` | unset (0) | deprecated field |
| `ChannelSettings.id` | unset (0) | we do not claim a global id |
| `uplink_enabled` / `downlink_enabled` | false | never bridge a crew to MQTT |
| `ModuleSettings.position_precision` | **32**, always explicitly present | the crew exists to find each other; see below |
| `Channel.index` | the primary slot, **0** | §4.2 |
| `Channel.role` | `PRIMARY` | |
| LoRa config (region, modem preset) | **never written** — this table is what a crew JOIN writes to the radio's channel table; it is unaffected by the §1.8 amendment below, which is about the SEPARATE "Copy Meshtastic link" export | §1.7 |

`position_precision = 32` is a deliberate departure from the import
path's safe default of 0 (`ChannelImportResult.makeChannelWritePlan`,
PR #274 review SHOULD-FIX 4). That default is right for a link a
stranger handed you; it is wrong here, where sharing an exact position
with these specific people is the entire product. The departure is paid
for in plain language, not silence — the join confirmation always says,
in the one sentence nobody has to expand: *"Your crew will see exactly
where you are."* The submessage is **always** emitted, never left absent
(absent means 32 on a write anyway — the trap `ChannelURL
.withExplicitPositionPrecision` exists to close — so saying it out loud
is free).

### 1.6 Threat model — stated honestly

**What the code protects against:** other people at the festival
casually seeing where your crew is, reading your crew's messages, or
showing up on your radar. That is the real, common threat, and 30 bits
handles it completely — nobody is guessing `FIRE-4K9M7X` by accident,
and a crew channel is invisible to anyone without it.

**What it does not protect against:** anyone who actually wants in.
There are 2^30 ≈ 1.07 × 10^9 possible codes. An attacker who captures
one encrypted crew packet can brute-force the entire space offline — one
HMAC-SHA256 pair plus one AES-CCM trial decrypt per candidate, which is
minutes to a few hours on a laptop and trivial on a GPU. The code is a
**privacy fence, not a security boundary.** Firefly must never claim
otherwise in copy, in docs, or in the README.

This is written into the app: Crew → Advanced → *About this crew's key*
says, in full, *"Your crew code keeps this crew private from other
people at the festival. It is not strong enough to stop someone who
really wants in. Don't put anything on here you'd mind a determined
stranger reading."*

**Collisions between crews.** Two independently minted codes collide
with probability 2^-30. A festival with 1 000 live Firefly crews has a
~4.7 x 10^-4 chance that *any* two of them share a code (birthday bound,
n^2/2N); at 10 000 crews it is ~4.7%. That is the honest number, and it
is a stated fact rather than something a test can assert — see AC4,
which used to try.

**Why not more bits.** 6 symbols is pinned by the 11-byte channel-name
limit (§1.3), and the `FIRE-` tag is worth its 5 characters: it is what
makes a code recognisable as a Firefly code on a puck screen, in a text
message, and in the stock Meshtastic app. If stronger is ever needed,
the escape hatch is already sized: **drop the `FIRE-` tag from the
channel name only** (keeping it in the UI), which buys 11 symbols = 55
bits with no change to anything else in this spec except §1.3's
identity between code and name. Recorded so the next person does not
have to redesign the format to get there.

Out of scope, listed so it is clearly a choice: PKI/encrypted DMs
(already cut in A01), per-member keys, key rotation, and revocation.
Removing someone from a crew, today and after this spec, means **starting
a new crew** — the Crew page says so in those words under Advanced.

### 1.7 Region and modem preset — never touched by a crew join

**Decision: Firefly never writes `lora_config` on a crew join, and never
auto-sets a region from the phone's locale.**

Three reasons, in order of weight:

1. **Locale is not location.** A phone bought in the US at a European
   festival reports `US`. Region selects the radio's transmit band; a
   wrong one is a regulatory violation and, more immediately, a radio
   that cannot hear the crew standing next to it. A silent guess that is
   wrong in exactly the case where the user can least diagnose it is the
   worst possible default.
2. **It is already set.** `docs/hardware/comms-brain.md` sets region at
   flash time, once, per radio. A crew join is not the moment to
   second-guess it.
3. **The honest path already exists.** `AdminWriteError.regionUnset` and
   `setRegion` landed in PR #274 precisely so `.unset` is never written
   by accident.

**What happens when the connected radio's region IS `UNSET`:** Start and
Join both stop, before any write, on a one-step blocking screen — not a
silent fix and not a dead end:

> **Your puck needs to know where you are**
> Radios use different frequencies in different countries. Pick yours
> once and your puck remembers it.
> [ United States ▾ ]   ← prefilled from your phone's region, change it if you're travelling
> [ SAVE AND CARRY ON ]

The picker is **prefilled** from `Locale.current.region` as a suggestion
the user confirms with a tap. That is the whole of "auto-set from
locale": a default in a control, never an applied write. It goes through
the existing `setRegion` + confirmation path.

Modem preset is never shown and never written on any path in this spec.
It is visible, read-only, under Crew → Advanced → Technical details.

### 1.8 QR and links

**The QR code contains a Firefly deep link. Decided.**

```
firefly://crew?v=1&code=FIRE-4K9M7X&name=Camp%20Firefly
```

- Parameter order is fixed (`v`, `code`, `name`) so the QR payload is
  byte-reproducible across platforms and testable by fixture.
- `v=1`. An unknown `v` shows *"This invite was made by a newer version
  of Firefly. Update the app to join."* — never a partial parse.
- `name` is optional and percent-encoded UTF-8, clamped to 24 characters
  after decoding. It is display text; it is never derived from and never
  affects the key.
- The scheme `firefly` is registered in `Info.plist`
  (`CFBundleURLTypes`) — new, the app has none today. Universal Links
  are out of scope (no server, and the whole product premise is that
  there is no signal).

**Why not a `meshtastic.org/e/#…` URL as the QR?** Because the QR is the
one thing Maya points a camera at, and it has to land her in Firefly's
own Join flow with the crew's *name* and one JOIN button — not in a
stock-Meshtastic import with channel-slot vocabulary, or in a browser.
The deep link is also ~55 characters versus ~110, which is a materially
easier scan on a scratched phone screen in the dark.

**But the Meshtastic link still exists, under Advanced.** Crew →
Advanced → **Copy Meshtastic link** produces the standard
`https://meshtastic.org/e/#<base64url ChannelSet>`, built by
`ChannelURL.encode` from the *same* derived channel, so a puck being
provisioned by CLI, a stock Meshtastic app, or any other client can
still import the crew. Its row says, in plain words, what it does:
*"For other apps and for setting up a puck by hand. Whatever imports
this link will use this crew as its main channel and turn its other
channels off."*

- The fallback URL is a **replace** URL (no `?add=true`). Replace,
  because the crew channel must land on index 0 as the primary
  everywhere (§4.2) and that is what a bare replace guarantees.
  `makeChannelWritePlan` already implements exactly this semantics and
  is reused unchanged for the *import* side (a phone importing a link
  it received).

> #### AMENDMENT — 2026-09-14, bench finding: the link MUST carry
> `lora_config`
>
> This subsection originally said the fallback URL carries **no**
> `lora_config`, "because §1.7 — the importer's region and preset are
> none of our business." That reasoning is correct for Firefly's OWN
> join path (admin `set_channel` never writes `lora_config` — §1.5,
> §1.7 stand, unaffected by this amendment) but **wrong** for this
> link, and shipping it that way would have broken every radio
> provisioned through it.
>
> **What's actually true, bench-confirmed on a Heltec V3:** the
> Meshtastic Python CLI's `--seturl` (and the official apps' URL
> import) **replace** the target radio's entire `lora_config` from the
> URL's `lora_config` field. An absent field is not "leave it alone" —
> it is "write an EMPTY `LoRaConfig`": `region UNSET`, `use_preset
> false`. `--seturl` against the old (no-`lora_config`) form of this
> URL left the bench radio unable to hear anything (`"region":
> "UNSET", "usePreset": false`); restoring a `--qr`-style URL that
> carries `lora_config` brought it back.
>
> **Decision:** the exported `ChannelSet` MUST carry `lora_config`,
> copied from the *exporting* radio's CURRENT LoRa config — at minimum
> `use_preset`, `modem_preset`, `region`, `hop_limit`, `tx_enabled`;
> never `channel_num`/`override_frequency` unless the exporting radio
> actually has them set (most don't — those two fields exist for
> advanced/test configurations this spec has no business inventing).
> This is never a guess: it is read straight off the connected radio's
> own `want_config` config replies (`NodeConfigSnapshot.loraConfig`,
> `FireflyMesh`), the same passive-read seam §1.7's region gate already
> uses.
>
> **Corollary: the export must refuse while the exporting radio's
> region is UNSET.** Copying an UNSET region forward would just move
> the deafness bug from "the link never carried a region" to "the link
> carries a *known-broken* one." Crew → Advanced → Copy Meshtastic link
> shows *"Set the radio region first"* and does not produce a link at
> all in that state, matching the same fact the Start/Join region gate
> already surfaces.
>
> This is a genuine scope change to what ships on the wire (the export
> path only — §1.5's join-write `ChannelSet` is untouched), so it gets
> the same test-vector treatment as everything else in §1.9: the
> fixture's `export_lora_config` block and every vector's
> `export_channelset_hex`/`meshtastic_url`, regenerated. §7 AC3 is
> amended to match.

### 1.9 Test vectors — byte exact

Every vector below is `code → PSK → ChannelSet bytes → meshtastic URL →
deep link`, and ships as a JSON fixture at
`docs/specs/fixtures/A02-crew-codes.json`, consumed by **both** the
Swift tests (`CrewCodeTests`) and the C tests
(`firmware/core/tests/test_crewcode.c`). One file, two languages, no
drift — the same discipline `ProtobufPinTests` uses.

The `ChannelSet` bytes (`channelset_hex`) are the serialization of
exactly one `ChannelSettings` (psk field 2, name field 3,
module_settings field 7 containing position_precision field 1 = 32), no
`lora_config` — this is the join-write `ChannelSet` (§1.5), unaffected
by the §1.8 amendment above.

**§1.8 amendment addition:** the fixture also carries a single
top-level `export_lora_config` block (the values the amendment's bench
example uses: `use_preset=true, modem_preset=LONG_FAST, region=US,
hop_limit=3, tx_enabled=true` — `ChannelSet.settings` field 1,
`lora_config` field 2; `LoRaConfig.use_preset` field 1, `modem_preset`
field 2, `region` field 7, `hop_limit` field 8, `tx_enabled` field 9,
verified against `MeshtasticProto/config.pb.swift`) and each vector adds
`export_channelset_hex` — `channelset_hex` with that `lora_config`
appended as field 2 — which is what `meshtastic_url` now encodes. Only
`channelset_hex`/`meshtastic_url` from before this amendment would have
diverged from each other; they do not, because `meshtastic_url` was
regenerated FROM `export_channelset_hex`, not from `channelset_hex`.

**Vector 1 — the canonical example (all shipped copy uses this code)**

| | |
|---|---|
| input | `FIRE-4K9M7X` |
| canonical | `FIRE-4K9M7X` |
| PSK (hex) | `743cc983ba326892fb91b6700b9ff3d08d7e002568b3f278db43244a249e71da` |
| PSK (base64) | `dDzJg7oyaJL7kbZwC5/z0I1+ACVos/J420MkSiSecdo=` |
| ChannelSet (hex) | `0a331220743cc983ba326892fb91b6700b9ff3d08d7e002568b3f278db43244a249e71da1a0b464952452d344b394d37583a020820` |
| Meshtastic URL | `https://meshtastic.org/e/#CjMSIHQ8yYO6MmiS-5G2cAuf89CNfgAlaLPyeNtDJEoknnHaGgtGSVJFLTRLOU03WDoCCCA` |
| deep link | `firefly://crew?v=1&code=FIRE-4K9M7X&name=Camp%20Firefly` |

**Vector 2 — all-zero symbols (lowest code)**

| | |
|---|---|
| input | `FIRE-000000` |
| PSK (hex) | `06d97b31396bffe249b7cab5c196a3b90d6c9880e2024963485bd69136d60fda` |
| ChannelSet (hex) | `0a33122006d97b31396bffe249b7cab5c196a3b90d6c9880e2024963485bd69136d60fda1a0b464952452d3030303030303a020820` |
| Meshtastic URL | `https://meshtastic.org/e/#CjMSIAbZezE5a__iSbfKtcGWo7kNbJiA4gJJY0hb1pE21g_aGgtGSVJFLTAwMDAwMDoCCCA` |

**Vector 3 — all-max symbols (highest code)**

| | |
|---|---|
| input | `FIRE-ZZZZZZ` |
| PSK (hex) | `fc832c6c532dc39fd509a1fed984610226d30faa2ec782696699eb11170f812c` |
| ChannelSet (hex) | `0a331220fc832c6c532dc39fd509a1fed984610226d30faa2ec782696699eb11170f812c1a0b464952452d5a5a5a5a5a5a3a020820` |
| Meshtastic URL | `https://meshtastic.org/e/#CjMSIPyDLGxTLcOf1Qmh_tmEYQIm0w-qLseCaWaZ6xEXD4EsGgtGSVJFLVpaWlpaWjoCCCA` |

**Vector 4 — normalisation, three spellings, one key.** `fire 4k9m7x`,
`4K9M7X` and `FIRE-4K9M7X` all canonicalise to `FIRE-4K9M7X` and derive
vector 1's PSK byte for byte.

**Vector 5 — Crockford aliasing is a real remap, not a no-op.**
`FIRE-4KIM7X` canonicalises to `FIRE-4K1M7X` (`I` → `1`) and derives
`5fd7aca5431d0162c27803fb2f507419b2c3eb132214618eae5e9b249caf46d4` —
**a different key from vector 1**, which is the point of pinning it: a
regression that dropped aliasing would silently put two people on two
crews.

**Vector 6 — rejections.** `FIRE-4K9M7`, `FIRE-4K9M7XY`, `FIRE-4K9M7U`
(`U` is rejected, never aliased), `FIRE-` and `""` all fail parsing.
No partial result, no fallback code.

> **Bench confirmation required before slice B merges.** Keeping the
> two apart, because only one of them is checkable here:
>
> *Verified in this checkout* (`app/FireflyKit/Sources/MeshtasticProto/`,
> the pinned generator's output): `ChannelSettings.name` is "A SHORT
> name that will be packed into the URL. **Less than 12 bytes**"
> (`channel.pb.swift`) — so `FIRE-` + 6 = 11 is exactly the budget.
> `MeshPacket.channel` delivered to a *client* is the **index**, not the
> hash: "Very briefly, while sending and receiving deep inside the
> device Router code, this field instead contains the 'channel hash'…
> This 'trick' is only used while the payload_variant is an 'encrypted'"
> (`mesh.pb.swift`). §4.1 clause 2 relies on that and is safe.
> `MeshPacket.via_mqtt` exists (field 14, same file).
>
> *NOT verified here*: that `Channels::generateHash` folds the channel
> **name** into the hash byte at all. That function lives in
> meshtastic/firmware, which this repo does not vendor. `channel.proto`'s
> "BobsChan" comment describes only the *display* letter
> (`0x41 + [xor all bytes of the psk] modulo 26`) and is **not**
> evidence for the on-air hash — do not cite it as if it were. So slice
> A's bench task is: set two radios to the same PSK with **different**
> names and confirm they cannot hear each other, then to the same name
> and confirm they can. If it comes back the other way, §1.3's identity
> is unnecessary but harmless and the spec stands unchanged.

---

## 2. Start a crew

**Entry points:** first-launch welcome (`CrewWelcome`), and Crew page →
Advanced → "Start a new crew" (which is also how you leave one crew for
a fresh one).

> **The puck can start a crew too — see `docs/specs/S02-core-crew.md`'s
> 2026-09-14 amendment, "slice D2 — the puck STARTS a crew".** This
> section assumes the organiser holds a phone. The festival topology is
> asymmetric and that assumption does not always hold: the person
> wearing the puck has no camera and a T9 keyboard, and the phone may be
> somebody else's. The puck therefore mints its own code, derives the
> PSK through the SAME `ff_crewcode` HKDF against the SAME fixture, and
> writes the channel to its own comms brain over Meshtastic's admin
> `set_channel` — then verifies it by re-reading the channel table
> before claiming success. Everything in §1 (the code, the key, the
> channel that gets written) and §1.7 (region is never written, never
> guessed) is shared byte for byte; only the client differs. The
> amendment owns the puck's UI, its failure vocabulary and its
> LEAVE/restore path.

**Screen: `CrewStartView`** (artboard `CrewStart.dc.html`)

```
Camp Firefly                        ← editable, tap to rename (local only)
Your crew · tap the name to change it

         ██ ▄▄ ██ ▄▄
         ▄▄ ██ ▄▄ ██           ← QR of the firefly:// deep link
         ██ ▄▄ ██ ▄▄

           FIRE-4K9M7X          ← large mono, selectable

Show this, or read the code out loud.
Anyone who scans or types it is in your crew.

[ Share link ]      [ Show on puck ]

Joined · 3                                     updates live
 T  Taylor        joined just now          IN
 D  Dana          joined 1 min ago         IN
 S  Sam           joined 2 min ago         IN
 ?  Waiting for more                       — 

[ Done · go to Find ]
```

### 2.1 Sequence

1. **Radio check.** If no radio is connected, the screen shows
   *"Finding your puck…"* and runs the existing `ConnectViewModel`
   discovery/connect path headlessly — never the Connect screen's own
   UI. Failure after 20 s offers *"Pick your puck yourself"* which
   pushes the (now radio-picker-only) Connect screen.
2. **Region check** (§1.7). Blocks here if `UNSET`.
3. **Mint.** 30 CSPRNG bits → canonical code → PSK (§1.4) → the channel
   of §1.5. Local state written **before** the radio write: the code,
   the human name (default `"My crew"`, immediately editable), and
   `crewCreatedAt` (the timestamp §2.3's "joined" list is measured
   from).
4. **Snapshot** the radio's current index-0 channel (name, PSK,
   precision) into the Keychain under
   `firefly.crew.preCrewPrimary.v1` — once only, and never overwritten
   by a Firefly crew channel. This is what Leave restores (§3.3).
5. **Confirm and write.** `AdminWriteConfirmationSheet`, reused as-is,
   with crew-flavoured copy (§2.2). On CONFIRM, the existing
   `preparePlan()` → `confirmApply()` path runs: a replace plan at index
   0 with no `lora_config`, read back and verified, exactly as PR #274
   specified. No new write machinery is introduced by this spec.
6. **Show the code** (the screen above) and start counting joiners
   (§2.3).

**Amendment (2026-09-14) — step 1 is a visible gate, not a silent
one.** Step 1 above said "the screen shows *Finding your puck…* and runs
the existing `ConnectViewModel` discovery/connect path headlessly". On a
first launch there is no remembered radio to connect to headlessly, so
that spinner was a spinner over nothing. What ships instead:

- **No radio → no attempt.** `CrewController.beginStart` /
  `beginJoin` refuse up front, before `preparePlan()` is called, and
  report `ApplyPhase.needsRadio`. The refusal is the only state this app
  can honestly promise touched the radio not at all.
- **A persistent banner**, on both Start and Join, whenever no puck is
  connected — *"Connect your puck to join / Firefly puts the crew on
  your puck itself, so your puck has to be connected first."* — with a
  **CONNECT** button that opens §6.1's connect step. Persistent, not a
  toast and not a post-tap error: the reason has to be on screen
  *before* the tap.
- **The primary action is disabled with its reason visible next to it.**
  A disabled button with no stated reason is the same bug with a greyer
  button.
- Coming back from the connect step with a puck connected **resumes the
  flow with no second tap** — the person already asked for a crew.

**Progress, and what each word actually means.** Start and Join both
render `CrewController.progressLabel`:

| Phase | Shown | What is actually running |
|---|---|---|
| `.checkingPuck` | "Checking your puck…" | `preparePlan()` reading the radio's live channel table |
| `.writing` | "Writing to your puck…" | `applyChannelSet()` — one indivisible call that writes AND makes the radio read its own values back |
| `.verifying` | "Checking…" | this app's own check of the returned report against the code it asked for |
| `.joined` | "Joined" | the crew profile is adopted |

The order differs from a naive "write, then check": the radio's write
and its read-back are one call in `MeshtasticClientProtocol`, so the app
cannot narrate the inside of it. It says "Writing to your puck…" for the
whole radio round trip and "Checking…" for the step it genuinely
performs itself. A fourth label describing something unobserved would be
invented progress.

### 2.2 The confirmation sheet, in plain language

The sheet is the existing `AdminWriteConfirmationSheet`; only the
`changes` lines and the footer change, and the technical lines move
behind a disclosure. Both reviews name this sheet as the place people
stop.

```
Start Camp Firefly?

This puts your puck on a private crew that only people
with your code can see. Your crew will see exactly where
you are.

Your puck will blink off for a few seconds while it saves
this, then reconnect on its own.

                                    Technical details ›
[ Not now ]                              [ Start crew ]
```

"Technical details" expands to the existing `ChannelApplySummary` lines
verbatim — `Firefly (index 0, primary) — precision 32 bits`, the
disabled/untouched slot lines, the region line when present. Nothing is
hidden from anyone who wants it; it is just not the default view. The
footer sentence replaces *"The node saves this, then reboots and
disconnects"* everywhere it appears, including the Settings name/region
writes that share this sheet.

### 2.3 "Joined N" — and why not "N of M" by default

The live list is the organiser's whole reason to stay on this screen.
Source of truth: `CrewJoinWatcher`, which lists every node admitted by
§4's rule **since `crewCreatedAt`**, newest first, with a per-row
"joined <age>" from the first admitting packet's receive time.

- The header reads **`Joined · 3`** by default. Not "3 of 8" — the app
  does not know how many people are coming.
- Tapping the header offers *"How many of you are there?"* (2–8). Once
  set, the header reads **`Joined · 3 of 8`** and a muted row reads
  **`Waiting for 5 more`**. `M` is editable and clearable at any time.
- **The waiting row never names anybody.** The artboard draws
  *"Marcus, Priya, Jo, Wes, Kim haven't scanned yet"* — the app has no
  possible source for those names before those people join, and
  inventing them is exactly what CLAUDE.md's honest-data rule forbids.
  An organiser who wants a named checklist can tap *"Add names"* and
  type them; only then are names shown, and they are rendered as a
  **local checklist, visibly separate** from the joined list, matched by
  nothing (a typed name is never auto-linked to a node — that would be
  a guess). Default: no names, `Waiting for 5 more`.
- A joiner with no NodeInfo yet renders as **"New crew member"** with the
  colour it was assigned, never a blank row (§4.4).
- **`Done · go to Find`** is always enabled. The screen never
  auto-advances on reaching `M` — an organiser watching people arrive
  should not have the screen yanked out from under them. (Deshawn's
  review sketched an auto-advance; this is a deliberate departure, and a
  cheap one.)

### 2.4 Share link / Show on puck

- **Share link** → the system share sheet with the `firefly://` deep
  link plus one line of text: *"Join my Firefly crew: FIRE-4K9M7X"*.
  The code is in the text on purpose, because half of these get pasted
  into a group chat that eats the link.
- **Show on puck** does **not** send anything. The phone and the puck
  are two clients of the *same* comms brain; they are not mesh peers of
  each other, so there is no packet the phone could address to the puck.
  The button opens a small sheet: *"On your puck: SETTINGS → CREW →
  SHOW CODE"*. The puck can always show the code without being asked,
  because it derives it from its own channel name (S02 amendment §D).
  A real remote trigger would need a new `ff_proto` message and is
  explicitly out of scope.

---

## 3. Join a crew

**Screen: `CrewJoinView`** (artboard `CrewJoin.dc.html`)

```
Join a crew
Scan the code your friend is showing

┌──────────────────────────┐
│  [ live camera preview ]  │
│  Camera · point at the QR │
└──────────────────────────┘

or type the code
FIRE-[_ _ _ _ _ _]
```

### 3.1 Scanning

Reuses the existing `QRScannerSheet`, inline rather than modal. It
accepts **three** payload shapes, in this order:

1. a `firefly://crew?…` deep link → the crew flow below;
2. a bare crew code (`FIRE-4K9M7X`, or any spelling §1.2 accepts) →
   the same flow, with no crew name;
3. a `https://meshtastic.org/e/#…` or `meshtastic://e/#…` URL → handed
   to the **existing** channel-import flow, with its existing
   confirmation sheet, reached and labelled as *"That's a Meshtastic
   channel link, not a Firefly crew code. Import it anyway?"* — kept
   because a puck provisioned by CLI is a real case, and refusing a
   valid link the user just scanned would be obtuse.

Anything else: *"That's not a Firefly crew code."* The camera keeps
running; a failed scan never dismisses the screen.

Camera permission string. The app **already ships**
`NSCameraUsageDescription` (`app/Firefly/Resources/Info.plist`), today
reading *"Firefly uses the camera to scan a crew's channel QR code."* —
which says "channel". It is **reworded**, not added, to: **"Firefly uses
the camera to scan your friend's crew code."** What *is* new is
`CFBundleURLTypes`: the repo has no URL-scheme registration at all
today, so §1.8's `firefly://` scheme is a genuine Info.plist addition.

### 3.2 Typing

A six-box mono field, `FIRE-` printed as a fixed prefix so nobody types
it. Each keystroke runs §1.2 normalisation, so `i` lands as `1` and `o`
as `0` *visibly, as you type* — the user sees the correction rather than
discovering it later. Autocapitalise all, no autocorrect, no
suggestions. The JOIN button enables on six valid symbols.

### 3.3 Joining

Same machinery as Start, minus the minting:

1. Radio check, region check (§1.7) — identical.
2. Derive the PSK from the code; build the same channel (§1.5).
3. Snapshot the pre-crew primary (§2.1 step 4) if this phone has never
   done it.
4. Confirmation sheet (artboard wording):

```
Join Camp Firefly?

Started by Deshawn · 3 people in so far.        ← only when the link said so
Your puck will blink off for a few seconds while
it saves this, then reconnect on its own.

                                    Technical details ›
[ Not now ]                                    [ Join ]
```

   > "Started by Deshawn · 3 people in so far" is drawn from the
   > artboard. **The app cannot know either fact before it joins** — the
   > deep link carries only `code` and `name`. Decision: this line is
   > **cut**, replaced by what is actually true and is enough:
   > *"Only people with this code can see this crew. Your crew will see
   > exactly where you are."* Adding an organiser name and a headcount
   > to the deep link was considered and rejected: both go stale the
   > moment the link is shared, and a stale headcount presented as fact
   > is the honesty failure this project keeps refusing to ship.

5. Write, read back, verify (existing path). On success the app lands
   on **Find**, Radar segment — crew members appear as their packets
   arrive, with no further action.
6. On failure, the honest `AdminWriteError` messages already written in
   `ChannelImportViewModel.writeMessage(for:)`, rephrased per §6 (no
   "node").

**Amendment (2026-09-14) — failure states, in full, and the one that
was missing.** §3.3's radio check (step 1) is the visible gate described
in the §2.1 amendment above; the same banner and the same disabled-with-
a-reason rule apply to JOIN. Every path out of an apply now ends
somewhere a person can read:

| What went wrong | What the screen says | Retry |
|---|---|---|
| No puck connected (refused before any attempt) | "Your puck isn't connected yet. Connect it, then try again." + the banner | the banner's CONNECT |
| Puck disconnected mid-write (`.notConnected`) | "Your puck isn't connected. Connect it, then try again." | CONFIRM / TRY AGAIN |
| NAK / partial apply (`.partialApplyFailed`) | "Couldn't send `<step>`: `<why>`. Your puck may be only partly set up — reconnect and try again." | CONFIRM / TRY AGAIN |
| Timeout (`.timeout`) | "Your puck didn't answer in time — it may still be restarting. Try again in a moment." | CONFIRM / TRY AGAIN |
| Read-back mismatch (`.readBackMismatch`) | "Your puck didn't confirm the change (`<detail>`). Nothing is certain until it does — try again." | CONFIRM / TRY AGAIN |
| Committed, not yet verified (`.committedButNotVerified`, 2026-09-14) | "Your puck restarted but hasn't come back yet — reconnect and Firefly will check the crew took." | none — Firefly settles it itself (below) |
| The app's own read-back check fails | "Your puck didn't come back with `FIRE-XXXXXX`. Nothing is certain until it does — try again." | CONFIRM / TRY AGAIN |
| Region `UNSET` (§1.7) | the existing `RegionGateView` | SAVE AND CARRY ON |

**None of these is ever a Swift enum case.** The shipped bug was exactly
that: `ChannelImportViewModel.planMessage(for:)` had no `AdminWriteError`
branch, so a not-connected radio during the PLAN step fell through its
`String(describing:)` default and the Join screen printed the bare word
`notConnected` on a `.footnote` alert-coloured line (`Color.ffAlert` —
styled like a real error, and still saying nothing), under a JOIN
button that stayed enabled. Pinned by `CrewControllerTests
.testJoinWithNoRadioNeverShowsARawEnumCase`.

**Leave (§3.4) is gated the same way.** Leaving is a write; refusing it
without a radio is what stops a "leave" from silently becoming a local
forget while the puck keeps transmitting precise positions on the crew
channel — §3.4's own worst possible outcome.

**Amendment (2026-09-14, bench) — a join that reached the puck is not
thrown away because the puck was slow to come back.** The row added to
the table above is the one state this flow had no answer for, and the
bench produced it on the first real run: the crew channel WAS written
(`meshtastic --info` afterwards: channel 0 = `FIRE-8MNTT2`, precision
32), `commit_edit_settings` rebooted the radio as it always does, the
link came back — and the app had already given up, said "Your puck
didn't answer in time", saved no profile, and left the phone claiming
no crew while the puck sat on one. A03 §3.6's amendment owns why the
waiting was wrong; this owns what the app does about it.

- **A third phase, not a failure.** `CrewController.ApplyPhase` gains
  `.awaitingPuck`, entered on and only on
  `AdminWriteError.committedButNotVerified`. It is not `.failed` — the
  write reached the puck and the puck did what a commit makes it do —
  and it is emphatically not `.joined`, because nothing has been read
  back. It shows the sentence in the table and no spinner: nothing is
  running.
- **Settled by a read-back, never by an assumption.** On the next
  `.ready` the app reads the puck's channel table **once**
  (`completePendingVerification()`) and moves to `.joined` only if the
  primary channel carries the crew by **name and key**
  (`CrewController.table(_:carries:)`). The name alone is public — it is
  printed on a screen and read out loud across a tent — so a puck
  sitting on a channel that merely shares the name is not this crew.
  Anything else is `.failed` with the existing "didn't come back with
  `FIRE-XXXXXX`" sentence.
- **A read, never a second write.** The recovery must not re-send the
  channel and reboot the puck again; that is the loop the bench's
  `.timeout` copy ("try again in a moment") would have invited, and it
  is why this row's Retry column is empty.
- **Held in memory, not persisted.** The pending read-back is
  meaningful only while this app is running and connected to the puck it
  wrote. A persisted one would come back after a relaunch as a claim
  about a radio that may since have been factory-reset or handed to
  someone else; a relaunch starts from what the puck actually says.
- **One attempt at a time.** A fresh `confirmApply()` supersedes a
  pending read-back, `cancelPending()` drops it, and `clearFailure()`
  deliberately does NOT — it only resets what is on screen, and
  `.awaitingPuck` is a live fact rather than a stale error.

Tests: `CrewControllerTests
.testAPuckThatRestartsAndComesBackLateStillJoinsAfterAReadBack`,
`.testAPuckThatComesBackOnADifferentCrewIsNeverClaimedAsAJoin`,
`.testAReadBackWithTheRightNameButTheWrongKeyIsNotThisCrew`,
`.testEveryOtherFailureEndsTheAttemptAndIsNeverResurrectedByAReconnect`.

### 3.4 Rejoin, change crew, leave

- **Rejoining the crew you are already on** (scanning your own code) is
  a no-op with a friendly confirmation: *"You're already in Camp
  Firefly."* No write is attempted.
- **Changing crews** is Join with a different code while already in one.
  One extra sentence in the sheet: *"You'll leave Camp Firefly and join
  Night Shift. You can come back with Camp Firefly's code."* The old
  crew's code and name are kept in a local "recent crews" list (max 4)
  so coming back is one tap, not a re-scan. Members, colours and hide
  lists are kept per crew code and restored on return.
- **Leave crew** (Crew page, bottom):

```
Leave Camp Firefly?

Your puck stops sharing your location with this crew,
and your crew stops showing up on your radar. You can
rejoin any time with the code FIRE-4K9M7X.

[ Cancel ]                              [ Leave crew ]
```

  **Decision: leaving writes the radio back, it is not a local
  forget.** A local-only leave would leave the radio still transmitting
  precise positions on the crew channel — the app would stop showing
  the crew while the crew kept seeing you, which is the worst possible
  outcome and a direct honesty failure. What gets written to index 0:

  1. the **pre-crew snapshot** from §2.1 step 4, if this phone has one;
  2. otherwise the **stock Meshtastic default primary**: empty name,
     single-byte PSK `0x01` (the documented "default key" shorthand,
     `channel.proto`), `position_precision = 0`. Precision 0 and not 32
     — a public default channel must never inherit the crew's
     exact-location setting. This is stated in the sheet's Technical
     details.

  Crew-local state (members, colours, hides, human name) is kept in the
  recent-crews list, not deleted, so a rejoin is not a fresh start.

- **Multiple crews at once: out of scope.** The radio has 8 channel
  slots and the model would work, but every screen in the app — Radar
  ring, Inbox, FIND, the crew colour palette — assumes one crew, and
  "which crew is this person in" is a question the whole UI would have
  to start answering. One crew at a time, switchable in two taps. Said
  out loud rather than left as an accident.

---

## 4. Auto-crew membership

### 4.1 The rule

> **A node becomes crew when this radio delivers us a packet it
> decrypted with our crew channel's key, from an id that is not us and
> not hidden, on one of four portnums.**

Precisely, all of the following, on the same packet:

1. It reached the client **decrypted**. A packet the radio could not
   decrypt never reaches a client at all, so this is a property of the
   delivery, not a check we perform — but it is the load-bearing one:
   possession of the PSK is what membership means.
2. `MeshPacket.channel` names the index our crew channel occupies on
   **this** radio, read from the live channel table
   (`currentChannelTable()`), not assumed. See §4.2.
3. `from != 0` and `from != connectedNodeNum`.
4. `from` is not on the hide list (§4.5).
5. `via_mqtt == false`.
6. The portnum is one of **`NODEINFO_APP` (4)**, **`POSITION_APP` (3)**,
   **`TEXT_MESSAGE_APP` (1)**, or **Firefly's own private portnum
   `FF_PORTNUM` = 269** (`firmware/core/include/ff_proto.h`).

   > **269 is not `PRIVATE_APP`.** In `portnums.proto` (verified in
   > this checkout, `MeshtasticProto/portnums.pb.swift`)
   > `PRIVATE_APP = 256` and `ATAK_FORWARDER = 257`; 269 is not a named
   > enumerator at all. It is a value Firefly picked inside the
   > documented private range 256–511, and it arrives on the wire as
   > `PortNum.UNRECOGNIZED(269)` — which `MeshtasticClient.handle(
   > meshPacket:)` already matches on `rawValue`, pinned by
   > `WireFormatTests.testFireflyPortnumSurvivesAsUnrecognized`. Every
   > implementation must match 269 by raw value; matching the
   > `PRIVATE_APP` case would silently admit nobody.

### 4.2 What does *not* admit anyone

- **Any other channel index**, including the public default. A friend
  chatting on LongFast is not crew.
- **MQTT-sourced packets** (`via_mqtt`). They may well carry our PSK if
  someone bridged the crew, but a crew is people who are *here*, and an
  MQTT path can replay. Excluded, and the exclusion is a test.
- **`TELEMETRY_APP` and everything else.** Telemetry refreshes presence
  for an existing member (`ff_crew_on_heard` already fires for any
  packet from a paired node — S02's 2026-09-07 amendment) but never
  admits a new one. Admission should ride on a packet type that carries
  identity or intent, and NodeInfo follows within minutes anyway.
- **The `want_config` NodeInfo replay.** This is the important one. The
  nodeDB dump is a synthesized snapshot, not a live `MeshPacket`, and it
  cannot prove the node was ever heard on *our* channel. It must not
  admit anyone, exactly as S02's 2026-09-07 amendment already ruled for
  presence: replay is not evidence.

  > **Not because "it carries no channel index"** — an earlier draft of
  > this spec said that and it is false. `NodeInfo` *does* have a
  > `channel` field (`mesh.proto` field 7, verified in this checkout):
  > *"local channel index we heard that node on. **Only populated if its
  > not the default channel**."* Which is precisely why it is useless
  > here: Firefly writes the crew channel at index 0, the primary, so
  > the field is left unset for exactly the nodes we care about and is
  > indistinguishable from unset-for-a-stranger-on-LongFast. It is also
  > a latched summary — the radio's memory of a past hearing, stamped by
  > a clock the summary itself defines — not an observation. Two
  > independent reasons; neither is "the field is missing."

  On the puck this falls out of routing admission through `on_rx_meta`,
  which the replay does not traverse. **In the app it does not fall out
  of anything**, and slice C has more work than "add a gate":

#### 4.2.1 What the app's client cannot tell us yet — slice C is `[api]`

Read before estimating slice C. Verified against
`app/FireflyKit/Sources/FireflyMesh/` on this branch's base:

1. **`MeshNodeSnapshot` carries no channel index and no `via_mqtt`.**
   It is the app's only per-node event
   (`MeshtasticClientProtocol.swift`), and clauses 2 and 5 of §4.1 are
   unimplementable from it. It must gain them — presence-flagged, so
   absent never reads as 0 — mirroring the `mc_rx_meta_t` addition
   S02's amendment already specifies for the puck. This makes **slice C
   an `[api]` PR**, same as slice D.
2. **`applyRxMeta(for:)` yields a snapshot for every packet from any
   non-zero sender**, on any channel, at any portnum, `via_mqtt` or
   not, and that snapshot flows into `CoreStore.apply(nodeUpdate:)` →
   `crew.onHeard`/`onRSSI` → `ff_crew_upsert`'s find-or-create. That,
   not a missing freshness check, is the live admission hole. (Since
   the 2026-09-11 bounded-unpaired-LRU amendment it can no longer
   *starve* paired members, which is the half of issue #266 that is
   closed; it still populates the roster with strangers, which is the
   half AC13 closes.)
3. **The four `crew.*` calls in `CoreStore.apply(nodeUpdate:)` are
   already conditional**, and deliberately so — each is gated on the
   datum being present and plausible, with the reasoning written out at
   length in that file (the honest-freshness three-tier rule from the
   hardening QA pass). Do **not** touch those conditions. The gate slice
   C adds is a *membership* gate in front of the whole function: a node
   that is neither already crew nor being admitted by §4.1 is not fed at
   all.
4. **There is no live `NODEINFO_APP` decode path.**
   `MeshtasticClient.handle(meshPacket:)` switches on
   `.positionApp`/`.routingApp`/`.adminApp`/`.textMessageApp` and
   raw-value 269, and `default: break`s everything else. Live NodeInfo
   *packets* are dropped; `.nodeInfo` only arrives on the `FromRadio`
   nodeDB path — i.e. the replay §4.1 refuses to admit from. So AC11's
   NodeInfo case has nothing to fire on today: slice C must add the
   `.nodeinfoApp` case. Until it does, a joiner is admitted by their
   first Position or Text, which is slower but not wrong.
5. Already fine: `IncomingText` and `IncomingPrivate` both carry
   `channel: pkt.channel` today, and `currentChannelTable()` exists and
   returns `[Channel]` with `index`, `settings.name` and `settings.psk`
   — so §4.2's name-and-PSK resolution is implementable from
   `want_config`'s Channel replies with no new API.

**Which index is "the crew index".** Normally 0 — that is what Firefly
writes (§1.5). But a radio provisioned by CLI or by the stock app (the
Advanced import path, §1.8) may hold the crew channel elsewhere, and
`MeshPacket.channel` is "inherently a local concept" (mesh.proto). So
the app resolves it once per connection: find the index in
`currentChannelTable()` whose `settings.name` equals our canonical code
**and** whose PSK matches our derived key; cache it for the connection;
re-resolve on reconnect and after any admin write. If no index matches,
the Crew page says so honestly — *"Your puck isn't on this crew's
channel"* with a **Fix it** button that re-runs the write — rather than
falling back to 0 and silently admitting strangers from the public
channel.

### 4.2.2 Ordering — admission happens-before the payload gate

> **Amendment, 2026-09-14 (bench).** Both halves of a packet are one
> event. **A packet's admission, computed from that packet's own rx
> facts (§4.1 clauses 1–6), happens-before any gate on that same
> packet's payload.** This is a required ORDERING, not a required set of
> checks, and it binds every implementation — puck and app alike.

The consequence, stated so it cannot be read as advisory: a receiver
that decides "is this sender crew?" for a FLARE, RALLY, STATUS,
FLARE_END, TEXT or PING **must** already have applied the admission the
very same packet earns. A sender who was not crew a moment ago and is
admitted BY this packet is crew for the purposes of this packet. There
is no second chance and no retry: the packet whose whole job is to
announce someone is precisely the packet that must not be dropped for
not knowing them yet.

**Why this is an amendment and not a clarification.** The puck already
had it structurally — `ff_shell.c`'s `shell_ev_rx_meta` runs
`shell_try_admit` off `on_rx_meta` **before** the portnum payload is
dispatched, in one synchronous call chain, so the ordering is not
something that could be got wrong there. The app had the same two steps
in the same order inside `MeshtasticClient.handle(meshPacket:)`
(`applyRxMeta(for:)`, then the decode switch) but published them on two
independent `EventHub`s, read by two independent `Task`s — `CoreStore`'s
`nodeUpdates()` loop, which admits, and `AppGraph`'s `incomingPrivate()`
loop, which gates. Two `AsyncStream`s have no happens-before between
them, so the ordering was not a property of the code at all; it was a
coin toss.

**Measured, on the bench, 2026-09-14 14:24** (Mac app on `d8ee569d`,
Heltec `TAY_06b0`, crew `FIRE-8MNTT2`). An unadmitted puck
(`!8f48af24` = 2403905316) sent a FLARE and then a text on the crew
channel:

```
[AppGraph] dropping inbound FLARE from unpaired/unknown sender=2403905316
```

…and the TEXT that followed was accepted and persisted as a crew
message. The FLARE — decrypted, on the crew channel, portnum 269, i.e.
admitting under §4.1 on every clause — was the ONE packet dropped, and
the text only got through because by then the flare's own rx facts had
finally landed on the other stream. `AdmissionBeforePayloadGateTests`
(`FireflyKit/Tests/FireflyModelTests`) pins the fixed behaviour for
FLARE, TEXT and RALLY.

**What satisfies this clause.** One ordered delivery per packet, whose
node facts and payload reach the deciding consumer in production order
— `FireflyMesh.InboundPacketEvent` / `MeshtasticClientProtocol
.inboundPackets()` in the app, the synchronous call chain on the puck.
What does **not** satisfy it: sleeping, retrying, re-queuing a dropped
payload, or a second admission check written into the gate. A second
implementation of §4.1 beside `CrewMembershipEngine` is forbidden by
this clause as surely as the wrong order is — the two halves must be
the same decision, made once, in order.

**This clause weakens nothing.** Applying a packet's facts first is not
admitting from the packet: hidden senders, ourselves, `via_mqtt`
packets, wrong-channel packets and non-admitting portnums are refused
exactly as §4.1/§4.2 already say, and the gate that follows still asks
the roster rather than the packet.

### 4.3 The cap, and what happens at nine

`FF_CREW_MAX` is **8**, and it stays 8 — on the puck for DRAM
(`firmware/tools/check_dram_budget.py`, `ff_crew_t`'s static arrays) and
**in the app too**, because the app's crew *is* `ff_crew_t` (`CrewStore`
over the C core). A Swift-side roster with a different cap would be a
second implementation of the model, which A01's own reuse table exists
to prevent.

Auto-membership makes the cap reachable by accident in a way manual
pairing never did: the 9th person who scans your code would, before this
spec, simply be dropped — `ff_crew_set_paired` returns `false` and
nothing happens. **That silent drop is not acceptable and this spec
forbids it.** Instead:

- The Crew page shows an explicit banner: *"2 more people are on this
  crew than your puck can track (8 is the limit). Hide someone to make
  room."*
- Those people are listed, honestly, under **"Not tracked (2)"** — with
  their name if NodeInfo arrived, `New crew member` otherwise, and
  their last-heard age. The overflow list is app-side, bounded, and
  LRU-evicted exactly like `NearbyNodesViewModel`'s own
  `maxUnpairedTracked = 64` dictionary, which this reuses.
- **Hiding someone frees a slot** (§4.5) and the oldest untracked member
  is admitted on their next qualifying packet.
- `CrewPairingController.pair` already returns `.full(limit:)` rather
  than failing silently; the auto-admission path routes through it
  unchanged and the banner is driven off that result.

Raising the cap is a real question for camps larger than eight, and the
answer is a core change (`FF_CREW_MAX`, the DRAM budget, `rssi_hist`'s
`[FF_CREW_MAX][FF_CREW_RSSI_HIST_CAP]`), not an app change. **Open
question, genuinely open** — see §8.

### 4.4 Naming fallback

A node admitted before its NodeInfo arrives has a colour and no name.
Both reviews flag today's behaviour (a blank circle with a `LINKED`
pill) as reading like a crash.

- Display name, in order: local nickname (`CrewPairingRecord.nickname`)
  → the radio's long name → the radio's short name → **`New crew
  member`**. Never blank, never a hex id, on any screen.
- The row's chip reads **`NAME?`** and its subtitle **`joined 2 min ago
  · no name yet`**.
- When NodeInfo lands, the name updates in place. The colour does not
  (assigned once at admission, `CrewColorAssignment.nextFreeIndex`,
  persisted — the existing rule).
- A member restored from persistence at launch, before any packet, reads
  **`waiting to hear from them`** with no presence chip. Not `NEVER`,
  not silence.

### 4.5 Hide

- **Hide is per node id, local to this phone, and never transmitted.**
  Nobody is told they were hidden; there is no "kick" on a mesh where
  possession of the key is membership, and pretending otherwise would
  be a lie about what the radio is doing.
- Implementation: **hide = `unpair` in `ff_crew` + the id on a persisted
  hide list.** This is deliberately the same mechanism as the cap (§4.3)
  — a hidden member frees a roster slot, which is the whole reason
  hiding is useful to a crew of nine.
- The admission rule (§4.1 clause 4) consults the hide list, so a hidden
  node is never silently re-admitted by its next packet.
- Stored per crew code (`firefly.crew.hidden.<code>.v1`), so leaving and
  rejoining a crew restores the hides you had.
- Effect: removed from the Radar ring, Map/Field pins, FIND targets, the
  Inbox conversation list, and the crew count (`People · 8 · 1 hidden`).
  **Their messages still arrive** — the thread is reachable from Crew →
  Hidden (N) → the person — but they raise no notification. Hiding is
  "off my radar", not "blocked"; the copy says so: *"They won't show on
  your radar. You'll still get their messages if they write."*
- Gesture: swipe a row on the Crew page → **Hide**. Undo toast, 5 s.
  Unhide from Crew → **Hidden (N)**.

### 4.6 Migration of manually-added crew

There are existing `CrewPairingRecord`s in
`firefly.settings.crewPairing.v1` on every phone that used M2's pairing.
Nothing about them is deleted.

- **No crew code stored yet** (every existing install): every existing
  paired record stays paired and visible, with its colour. The Crew page
  shows a one-time banner: *"Your crew came from an older version of
  Firefly. Start or join a crew to get a code everyone can scan."* The
  crew has no code, no QR, and no Share — those rows are absent, not
  disabled-with-no-reason.
- **After a Start or Join**, pre-existing records that are *not* seen on
  the crew channel are **grandfathered, never auto-removed**: they keep
  their slot, their colour and their honest presence, listed under
  **"From before"** on the Crew page with a Hide action. Auto-removing
  them would silently delete the user's crew on upgrade, which is worse
  than a slightly cluttered list. They count against the 8.
- Colours are preserved from the persisted record in every case; the
  record remains authoritative (`CrewPairingRestorer`, unchanged).

### 4.7 What the "Add crew" sheet becomes

It stops being an add sheet, because there is nothing to add — having
the code *is* membership, and someone who does not have it cannot be
added by tapping. Connect's NEARBY section and Settings' CREW → ADD row
are both removed.

In their place, Crew → Advanced → **People my puck hears** lists nodes
the radio has heard that are **not** on the crew channel
(`ff_heard_t`'s role on the puck; the app's own bounded dictionary),
with exactly two actions per row: **Name** (a local nickname) and
**Hide** (never show this id anywhere, including here). No Add. The
section's caption says why: *"These radios aren't in your crew — they're
just nearby. To get someone into your crew, send them your code."*

---

## 5. The Crew page

**Screen: `CrewScreen`** (artboard `CrewPage.dc.html`), reached from
More → Crew, and the destination of Start/Join.

```
Crew
Camp Firefly · code FIRE-4K9M7X

┌─────────────────────────────────────────┐
│ Your puck · Jake                        │
│ Connected · battery 82%      [Connected]│
└─────────────────────────────────────────┘

People · 8                          [ Show code ]
 T  Taylor   heard just now · near the Tower        HERE
 D  Dana     heard 40 s ago                         HERE
 S  Sam      quiet for 6 min                        QUIET
 M  Marcus   not heard since 9:40 pm ·
             last seen by the Crater            NO SIGNAL
 ?  New crew member  joined 2 min ago · no name yet  NAME?

Anyone with the code is in. Swipe a person to hide them
from your radar.

Advanced          radio, frequency, invite link ›
Leave crew
```

- **Show code** pushes the same QR + code panel Start ends on, minus the
  mint — one screen, two entry points.
- `near the Tower` / `by the Crater` is festpack POI proximity on the
  member's **last known** position, rendered only when a position exists
  and only alongside its own age. Never as a standalone claim of where
  someone is.
- **`Your puck` row** is the only radio on the main path, and it says
  nothing technical: name, connected/not, battery. Tapping it goes to
  Advanced → the radio picker.

---

## 6. Copy and states

The rule, from both reviews: **on the main path the product has pucks,
crews and people. It does not have nodes, channels, regions, presets,
indexes, PSKs, acks, dBm, SNR or Meshtastic.** Every one of those words
still exists — under Advanced, spelled correctly, for the person who
wants it.

### 6.1 First launch (`CrewWelcome.dc.html`)

```
Find your people
No signal needed. Your puck talks to your crew's pucks
directly.

[ Start a crew ]
[ Join a crew ]

One person starts the crew and shows a code. Everyone
else scans it. That's the whole setup.

Already set up? Connect your puck
```

This replaces "launch lands on More with Connect pre-pushed" (A01,
Navigation → First launch). The condition is unchanged — no known radio
*or* no crew — but the destination is this screen, not the radio picker.
"Connect your puck" is the escape hatch to the radio picker for someone
re-installing.

**Amendment (2026-09-14) — the flow gains a connect step: Welcome →
Connect your puck → Start / Join.** Owner report, build 328 on the
iPhone: *"Tried to join but nothing happened, still on the Join a crew
screen. We also need better handling on that screen for connecting to a
puck/meshtastic node first or making sure we are connected. Overall
better onboarding."* Both halves of §2.1 step 1 and §3.3 step 1 assumed
the radio check could stay invisible ("runs the existing discovery/
connect path headlessly"); in practice a first launch has no remembered
radio to connect to headlessly, and the invisible check turned into a
screen that appeared to do nothing.

- **Screen: `CrewConnectPuckView`**, pushed between Welcome and
  Start/Join whenever `CrewController.hasConnectedRadio` is `false`, and
  **skipped entirely when it is `true`** — a person whose puck is
  already connected never sees it.

```
Connect your puck
Turn your puck on and hold it near your phone. Firefly needs it
connected before you can start or join a crew.

● Not connected                         ← / Connecting to X… / Setting
                                          up X… / Connected to X
 Meshtastic_e7d4   e7d4        CONNECT
[ RESCAN ]

Don't have a puck yet?
Do this later
```

- It is **not a second Connect screen**: it drives the same
  `ConnectViewModel` and the same `RadioListBuilder` rows through the
  same `PeripheralDiscovering` seam. What differs is what it says (A02
  §6's vocabulary — no node id, no dBm, no channel card, no NEARBY list)
  and two behaviours the Connect screen deliberately does not have:
  1. **It scans on appear.** `ConnectScreen` must not (merely showing a
     screen is not the moment to make the OS put up its Bluetooth
     dialog); here the user has just tapped a button that says they want
     to connect their puck.
  2. **It auto-connects to the remembered radio once**, with no tap —
     §2.1 step 1's headless path, kept, for the case where it actually
     applies.
- **Bluetooth off / not allowed / unsupported** are three separate,
  honest sentences (`ConnectViewModel.RadioTrouble.plainMessage`),
  classified from a typed `BluetoothUnavailable` error. Before this they
  all reached the screen as `String(describing:)` output — literally the
  word `notConnected` for "Bluetooth is off".
- **"Do this later" is never removed and never disabled**, and it
  advances to Start/Join rather than dead-ending: the destination's own
  banner (§2/§3 amendment below) then explains in place.
- **"Don't have a puck yet?"** opens a short plain-language explainer.
  It does not sell anything; it says what a puck is and what the app can
  and cannot do without one.

### 6.2 Permission strings (`Info.plist`)

| Key | New string |
|---|---|
| `NSBluetoothAlwaysUsageDescription` | Firefly connects to your puck over Bluetooth so you can see your crew and send messages. |
| `NSBluetoothPeripheralUsageDescription` | (same) |
| `NSLocationWhenInUseUsageDescription` | Firefly shares your location with your puck so your crew can find you, even with no signal. |
| `NSLocationAlwaysAndWhenInUseUsageDescription` | (same, plus) Keeping this on in the background means your crew can still find you while your phone is in your pocket. |
| `NSCameraUsageDescription` *(reworded — the key already ships, saying "channel")* | Firefly uses the camera to scan your friend's crew code. |
| `CFBundleURLTypes` *(new — the app registers no URL scheme today)* | the `firefly` scheme, §1.8 |

### 6.3 Presence and status words — one vocabulary, both surfaces

**Amendment (2026-09-14, owner decision via the orchestrator) —
supersedes this section as originally written.** The QUIET / `quiet for
6 min` / `not heard since 9:40 pm` / `waiting to hear from them` copy
below, and the `NAME?` chip in §4.4, predate two PRs that have since
shipped and are canonical:

- **App** — PR #304 (`e9d2ad0`, "app: plain-language states and
  delivery words, puck permission strings, FIND persists across
  segments"), pinned by `PresenceWordsTests.swift`.
- **Puck** — PR #303 (`dbbc79b`, "puck: plain-language faces — status
  words, no jargon outside Diagnostics").

Where this section's original wording disagrees with what shipped, the
table below wins. `ff_crew_presence_t`'s enum names (`HEARD`/`STALE`/
`LOST`/`NEVER`, S02's 2026-09-07 amendment) are untouched — only what
each state renders as changed.

One vocabulary, everywhere it appears — puck (Radar, Inbox/Signals
rows, Crew page) and app (Crew page, Inbox rows, Radar detail line) —
always paired with an age where an age is honestly known:

| State | Puck word | App word | When |
|---|---|---|---|
| Heard recently | `SEEN <age>` | "Heard just now" / "`<age>` ago" | < 2 min since any packet heard (`HEARD`) |
| Heard, aging | `SEEN <age>` | age only, e.g. "6 min ago" | 2–10 min since any packet heard (`STALE`) |
| Long radio silence | `NO SIGNAL <age>` | "No signal · 40 min" | > 10 min since any packet heard (`LOST`) |
| Paired, never heard | `NOT SEEN YET` | "Paired · not seen yet" | paired but zero packets ever received (`NEVER`) |
| No GPS fix, never heard | `NO LOCATION YET` | "No location yet" | selection has no position, and is never-heard |
| No GPS fix, heard recently | `NEARBY, NO LOCATION` | "Near · no location yet" | selection has no position, but IS heard/stale |
| Relayed | `RELAYED` | "via relay `<name>`" — only when true | packet reached this puck/app through another node, not directly |
| Sending | `WAITING` (queued) / `SENT` (accepted by the radio) | "Sending…" | outgoing message queued or accepted, not yet resolved |
| Delivered | `DELIVERED` | "Delivered" | a routing ack came back OK for a direct send |
| Not delivered | `NOT DELIVERED` | "Didn't get through" (RESEND unchanged) | routing NAK, or ack timeout, on a direct send |
| Not sent | `NOT SENT` | "Couldn't send" / "Couldn't send · `<reason>`" | evicted from the bounded outbox queue before it could send |
| Nameless crew member | *(no puck equivalent — see below)* | "New crew member" | crew member paired with an empty/blank name |
| Puck↔radio link *(not presence — a different axis)* | `LINKED` / `NO RADIO` | *(app-only concept is Bluetooth-to-puck, a different axis; no shared word)* | the puck's own connection to its comms-brain radio |

"LOST" is retired from the user-facing vocabulary entirely, on both
surfaces — Deshawn's original review (which this section's now-retired
draft was trying to satisfy) is still right that it reads as *the
person* is lost, at 11pm, to someone worried about a friend. The
`ff_crew_presence_t` enum keeps the name `LOST`; no screen renders it.

The **no-signal** detail view (puck and app both) adds the calm, honest
guidance Deshawn asked for: *"Could be a dead battery, out of range, or
turned off."* plus a **[ SEND RALLY ]** action, because that is the
actual next step. Nothing there is invented — battery/range/off are the
three possible causes, stated as possibilities, never picked between.

**§4.4 correction, same amendment:** the shipped nameless-row fallback
is **`New crew member`** for the display name and a **`?`** in the
colour swatch (`CrewMemberRow`, owner decision 2026-09-13) — not the
`NAME?` chip or the `waiting to hear from them` / `joined 2 min ago ·
no name yet` subtitle §4.4 describes. A member restored from
persistence before any packet reads **"Paired · not seen yet"** (this
table's `NEVER` row), not a bespoke phrase.

### 6.4 Other copy replacements

Presence, delivery, relay, and nameless-row words move to the shared
table in §6.3. What remains here:

| Today | Ships as |
|---|---|
| `LINKED` chip on a nameless row | never occurs: `New crew member` + a `?` swatch initial (§6.3 correction to §4.4) |
| "The node saves this, then reboots and disconnects…" | "Your puck will blink off for a few seconds while it saves this, then reconnect on its own." |
| "Tap RESCAN to look for nearby Meshtastic radios" | "Tap RESCAN to find your puck." |
| Settings "NODE NAME" / "APPLY NAME TO NODE" | "Your name" / "This is what your crew sees for you." / **Save** |
| "Taylor's radio: strong signal, direct, heard just now" | "Taylor: strong signal, heard just now" — `relayed through Dana` appended **only when true**, never a bare "direct" (§6.3's `RELAYED` / `via relay` row) |
| `−61 dBm`, `SNR 4.2 dB` on any main-path screen | removed; Advanced only |
| `!02e5e3d4` | removed from the main path; Advanced only |

**Amendment (2026-09-14, bench) — the words for a puck that restarted
and hasn't come back.** `AdminWriteError.committedButNotVerified`
(§3.3's amendment) ships as:

> "Your puck restarted but hasn't come back yet — reconnect and Firefly
> will check the crew took."

Three deliberate choices. It does **not** reuse the `.timeout` sentence
("didn't answer in time"), because the puck did answer — it committed
and restarted. It does **not** end in "try again", because trying again
means writing the same channel and rebooting the puck a second time. And
it names what happens next, because something does: Firefly settles it
with a read-back on its own.

Callers outside a crew join pass their own subject, so the sentence
stays true for them — Settings' name write says "check your name took",
its region write "check the band took"
(`ChannelImportViewModel.writeMessage(for:subject:)`). A name write must
never tell a user Firefly is about to check a crew.

**Amendment (2026-09-14) — `±6 m` stays.** The row this replaced also
deleted GPS accuracy (`±6 m`) from main-path screens. Reversed by owner
decision: GPS accuracy is a fact, not jargon, and Radar keeps showing
it. Only the raw radio numbers (dBm, SNR) and the raw node id move to
Advanced.

### 6.5 Advanced — the full inventory

One section, on the Crew page, labelled `Advanced · radio, frequency,
invite link ›`. Everything the reviews asked to hide lives here and
nowhere else:

- **Your puck** → the radio picker (see §6.6), disconnect/forget, BLE
  name, node id, firmware version, link uptime
- **Frequency band (region)** → the existing picker + `setRegion`
- **Technical details** → channel index, channel name (= the code),
  modem preset (read-only), position precision, PSK fingerprint (first
  4 bytes, hex — never the key itself)
- **Copy Meshtastic link** (§1.8) and **Import a channel link** (the
  existing `ChannelImportViewModel` flow, unchanged)
- **People my puck hears** (§4.7)
- **Hidden (N)** (§4.5)
- **About this crew's key** — the threat-model paragraph, §1.6, in full
- **Start a new crew** (mints a fresh code; leaves the current crew)
- **System / diagnostics** — the existing screen, moved here from More

### 6.6 What Connect becomes

**A radio picker, and nothing else.** RESCAN, the discovered-peripheral
list, connect/disconnect/forget, and the link-state line. Its title
becomes **"Your puck"**.

Removed from it: the CHANNEL card (→ Advanced → Import a channel link)
and the NEARBY section with its ADD TO CREW buttons (→ deleted, §4.7).

Reached from exactly two places: Crew → Advanced → Your puck, and the
first-launch welcome's "Already set up? Connect your puck". It is no
longer a launch destination and no longer a tab-adjacent row people
stumble into.

### 6.7 FIND keeps running across Find segments

Owner decision, 2026-09-13, and a correction to A01's current
`RootView.applyFindLifecycle()` rule ("only the visible segment's view
model observes/pumps"). That rule is right for *rendering* pumps and
wrong for a **live FIND session**: switching Radar → Map to see where
someone is, and losing the FIND you started, is a bug with a rationale.

- An active `ff_find` session and its ping cadence are owned by
  `AppGraph`, not by any segment's view model, and keep running across
  segment switches, tab switches, and backgrounding (subject to the
  existing `backgroundConnectEnabled` setting, unchanged).
- The per-segment start/stop rule still applies to Radar's 1 Hz
  recompute and Map's pin-refresh loop.
- Every Find segment shows the same FIND banner while a session is live,
  so it is never invisible: `FINDING TAYLOR · 465 ft · warmer`, with
  STOP.
- `FindLifecycleWiringGuardTests` gains a case pinning this.

---

## 7. Acceptance criteria

**Codec (shared, C and Swift)**

1. **A02_AC1** — `CrewCode.parse` accepts all of `FIRE-4K9M7X`,
   `fire 4k9m7x`, `4K9M7X`, `FIRE-4KIM7X` and returns the canonical form
   for each (vectors 4 and 5); rejects every string in vector 6 with a
   typed error and no partial result. `U` is rejected, not aliased.
2. **A02_AC2** — `CrewCode.psk(for:)` reproduces vectors 1–3 and 5 byte
   for byte, in both the Swift and C implementations, from the shared
   `docs/specs/fixtures/A02-crew-codes.json`.
3. **A02_AC3** — the join-write `ChannelSet` (`CrewChannel
   .channelSet(for:)`, §1.5) serializes to the vector's
   `channelset_hex` exactly. `position_precision` is present and 32 in
   every case; `lora_config` is absent in every case — this is the
   `ChannelSet` Firefly's own join path writes, and the §1.8 amendment
   does not touch it.
   **§1.8 amendment, 2026-09-14** — the EXPORT `ChannelSet`
   (`CrewChannel.exportChannelSet(for:loraConfig:)`, built with the
   fixture's `export_lora_config`) serializes to `export_channelset_hex`
   exactly, `hasLoraConfig` is `true`, and `ChannelURL.encode` of it
   produces the vector's `meshtastic_url` exactly; `ChannelURL.parse` of
   that URL round-trips to an identical export `ChannelSet`. Exporting
   with `region == .unset` throws (`CrewChannel.ExportError
   .regionUnset`) rather than producing a URL.
4. **A02_AC4** — code generation is correct **and its test is
   deterministic**. Three parts, none of them a sampling test:
   (a) generation draws its 30 bits from the platform CSPRNG
   (`SecRandomCopyBytes` / `esp_random`) and never from a seeded,
   time-derived or node-derived source — pinned by injecting a
   recording randomness source, not by statistics;
   (b) the 30-bit integer → code encoding is a **bijection**, pinned at
   both endpoints by the fixture (`0` → `FIRE-000000`, `2^30 - 1` →
   `FIRE-ZZZZZZ`) and by an exhaustive per-position sweep: for each of
   the 6 positions, all 32 symbols are reachable and each maps back to
   exactly its own 5 bits, so no symbol is ever favoured;
   (c) `parse(generate())` round-trips to the generated code, and every
   generated code is 6 symbols all drawn from the alphabet.

   > **The obvious version of this criterion is broken and was in this
   > spec until review.** It read: *"over 100 000 generated codes yields
   > no duplicates and a per-symbol distribution within 1% of uniform."*
   > Both halves fail against a **perfectly uniform CSPRNG**. Measured,
   > 20 trials of 100 000 codes from `secrets.randbits(30)`: a duplicate
   > appeared in **20/20** (birthday bound over a 2^30 space predicts
   > `1 - e^(-n^2/2N)` = 99.05%), and "every symbol within 1% of
   > uniform" passed **0/20** (1% of the per-symbol mean 18 750 is
   > 1.39σ, and 32 buckets must all land inside it: ~0.3%). This is the
   > house proxy-check failure (`docs/review/code-review.md` item 6) in
   > its purest form — a test whose passing measures luck, and here
   > mostly measures bad luck. Collision probability is a **property of
   > 30 bits**, stated as a fact in §1.6, not something a test run can
   > assert.
5. **A02_AC5** — the deep link round-trips: `CrewInvite.encode` produces
   vector 1's link byte for byte (parameter order fixed); `parse`
   rejects `v=2`, a missing `code`, a malformed code, and a `name`
   longer than 24 decoded characters (clamped, not rejected).

**Start / Join**

6. **A02_AC6** — Start mints a code, snapshots the pre-crew primary
   exactly once, and calls `applyChannelSet` with a replace plan at
   index 0, `role == .primary`, `loraConfig == nil`. A second Start
   never overwrites the snapshot.
7. **A02_AC7** — with the radio's region `UNSET`, both Start and Join
   stop before any write, show the region step prefilled from
   `Locale.current.region`, and write the region only after the user
   confirms. `setRegion(.unset)` is never called.
8. **A02_AC8** — the confirmation sheet's default body contains no
   occurrence of "node", "channel", "index", "precision", "preset",
   "region", "PSK" or "Meshtastic"; expanding Technical details reveals
   the existing `ChannelApplySummary` lines unchanged. (Enforced as a
   string test over the rendered copy, not by eye.)
9. **A02_AC9** — Join accepts a deep link, a bare code and a
   `meshtastic.org/e/#…` URL, routing the third to the import flow; a
   junk payload leaves the camera running and dismisses nothing.
10. **A02_AC10** — rejoining your own code writes nothing; changing
    crews warns by name and preserves the old crew's members, colours
    and hides in the recent-crews list; leaving writes the snapshot back,
    or the stock default primary with `position_precision == 0` when
    there is no snapshot.

**Auto-membership**

11. **A02_AC11** ✅ (slice C, PR pending) — a decrypted `NODEINFO_APP`/`POSITION_APP`/
    `TEXT_MESSAGE_APP`/`FF_PORTNUM` (269, matched by raw value — §4.1
    clause 6) packet on the crew index from an unknown id admits that id
    as crew, assigns the next free colour, and persists a
    `CrewPairingRecord` — without any user action.
12. **A02_AC12** ✅ (slice C) — none of the following admit anyone: the same packet
    on another channel index; the same packet with `via_mqtt == true`; a
    `TELEMETRY_APP` packet; a `want_config` NodeInfo replay entry; a
    packet from our own `connectedNodeNum`; a packet from a hidden id.
    Each is its own test.
13. **A02_AC13** ✅ (slice C) — a **membership gate in front of**
    `CoreStore.apply(nodeUpdate:)` drops any node that is neither
    already crew nor being admitted by AC11, so a replayed nodeDB of 200
    strangers leaves `ff_crew` untouched (issue #266's app-side live
    exposure — the core half landed 2026-09-11 and the issue is closed;
    this is the remaining exposure, §4.2.1 item 2). The four existing
    `crew.*` conditions inside that function are **unchanged** — a test
    asserts a crew member with an implausible timestamp is still not fed
    (§4.2.1 item 3), so the gate cannot be "fixed" by loosening them.
    `Nearby`'s own dictionary (`NearbyNodesViewModel`, its own
    `nodeUpdates()` subscription) is untouched, so §4.7's "People my
    puck hears" still has its data.
14. **A02_AC14** ✅ (slice C) — crew index resolution matches on name **and** PSK,
    re-resolves on reconnect, and when no index matches, the Crew page
    shows "Your puck isn't on this crew's channel" and admits nobody —
    it never falls back to index 0.
15. **A02_AC15** ◐ (slice C: engine + overflow list; the Crew page's banner is slice B) — the 9th distinct joiner is never silently dropped:
    the Crew page reports the overflow count and lists the untracked
    members; hiding a member frees the slot and the oldest untracked
    member is admitted on its next qualifying packet.
16. **A02_AC16** ◐ (slice C: unpair + persisted hide + no re-admission; the Radar/Map/Inbox/FIND and notification halves are slice B/E) — hide unpairs in `ff_crew`, persists per crew code,
    survives relaunch, blocks re-admission, removes the member from
    Radar/Map/Inbox/FIND and the count, keeps their thread reachable
    under Hidden, and raises no notification. Unhide restores the member
    on their next packet.
17. **A02_AC17** ◐ (slice C: nothing removed, origin exposed; the banner and the "From before" section are slice B) — migration: an install with existing
    `CrewPairingRecord`s and no crew code keeps every member with its
    colour, shows the one-time banner, and offers no QR/Share; after a
    Join, unseen pre-existing members appear under "From before" and are
    never auto-removed.

**Copy and states**

18. **A02_AC18** — no screen on the main path renders a blank display
    name: with NodeInfo absent, every crew row reads `New crew member`
    with a colour and a `NAME?` chip; with NodeInfo present, the name
    updates in place and the colour does not change.
19. **A02_AC19** — presence copy matches §6.3 exactly at the 2 min and
    10 min boundaries (inclusive toward STALE, S02's convention); the
    string "LOST" appears in no user-facing string in the app bundle
    (enforced by a test over `Localizable`/literal copy). Scope it to
    **rendered** copy: `InboxViewModel`'s `case lost = "LOST"` is an
    enum raw value and stays — the criterion is that no view renders it,
    not that the four letters vanish from the source.
20. **A02_AC20** — "NO ACK" appears in no **rendered** string; the tag
    reads "Didn't get through" and keeps its RESEND. Same scoping as
    AC19 and for the same reason: `InboxListView`'s
    `case .noAck: return "NO ACK"` is the one line that changes, while
    the phrase stays in doc comments in `MeshtasticClient.swift` and
    `DemoMeshtasticClient.swift` that describe the delivery *state
    machine*. A test that greps the whole source tree fails on those
    comments and teaches the next person to delete documentation.
21. **A02_AC21** — starting a FIND, then switching Radar → Map → Field →
    Inbox → back, leaves the session running with unchanged elapsed
    time and ping cadence, and the FIND banner visible on every Find
    segment.
22. **A02_AC22** — Connect renders only the radio picker; it contains no
    channel-import control and no ADD TO CREW control.

---

---

### Slice C implementation notes and deviations (2026-09-13)

Recorded here rather than only in the PR body, because each is a place
the implementation is not literally what a sentence above says.

1. **`MeshNodeSnapshot` gained one optional `rxMeta: MeshRxMeta?`, not a
   presence-flagged channel index.** §4.2.1 item 1 asks for the channel
   index "presence-flagged, so absent never reads as 0", mirroring
   `mc_rx_meta_t`'s `has_channel_index`. Presence is carried by the
   optionality of the WHOLE struct instead, and that is the more honest
   shape on this side: a snapshot with no `rxMeta` is a want_config
   replay entry, which has no channel field to misread at all, while a
   snapshot WITH one came off a real `MeshPacket` — where `channel == 0`
   is not "absent" but the primary, which is exactly where Firefly
   writes the crew channel (§1.5). `MeshPacket.channel` has proto3
   implicit presence, so a per-field flag here could only ever have been
   invented rather than read.

2. **`applyRxMeta` now publishes a snapshot for EVERY packet naming a
   sender**, not only for one whose RSSI could be attributed to a node
   the nodeDB already knew. Without this, §4.2.1 item 2's hole is only
   half closed in the other direction: a `TEXT_MESSAGE_APP` or
   portnum-269 packet from an id with no nodeDB record produced no
   `nodeUpdates()` element at all, so AC11's text and 269 cases had
   nothing to fire on. The published snapshot carries the node's
   EXISTING record (never a blank one — a consumer that replaces by
   `num` must not lose a name), plus the packet facts.

3. **"New crew member" is a display fallback, never written into
   `ff_crew`.** §4.4 already describes it as a display-name order, and
   `CrewMembershipEngine.displayName(nickname:longName:shortName:)` is
   where it lives. Writing it into `ff_crew_member_t.long_name` at
   admission would put a fabricated name in the model that a later real
   NodeInfo could not be distinguished from.

4. **The gate is installed, not built in.** `CoreStore.membership` is
   `nil` by default, which keeps the pre-A02 behaviour for compositions
   that have no crew at all (`CoreStoreTests`, headless consumers);
   `AppGraph` always installs one, before `start()` and after
   `CrewPairingRestorer.restore`. A gate that dropped everything for a
   composition with no membership policy would be a behaviour change
   dressed up as a safety measure.

5. **AC15's "the OLDEST untracked member is admitted" is packet-driven,
   not queued.** Freeing a slot admits whichever untracked member sends
   the next qualifying packet; there is no ordering pass that picks the
   oldest, because admission has no event of its own to run on. The
   observable promise — a freed slot is taken, and nobody is silently
   dropped — is what the test pins.

6. **`CrewMembershipProviding` is slice B's, not slice C's.** Slice B
   had already declared it (`currentMembers() -> [CrewJoinedMember]`,
   plus the `PairingCrewMembershipProvider` stub that stands in until
   this engine is wired), so slice C carries that file byte-for-byte
   rather than declaring a second protocol of the same name, and
   `CrewMembershipEngine` conforms to it — sorting newest-join-first per
   §2.3, with a total order so two admissions inside one millisecond
   cannot come back in a different order run to run. The gate is its own
   one-method protocol (`CrewMembershipGating`), because `CoreStore`
   asks one question and should not be able to see the UI's readout.

7. **Attribution rides on the packet, not on the node record.**
   Added in review. Widening `applyRxMeta` (note 2) means a crew
   member's MQTT-bridged and multi-hop packets now reach
   `CoreStore.apply(nodeUpdate:)`, carrying the node's EXISTING record —
   whose `hopsAway`/`rssiDbm` are the nodeDB's latched summary of an
   earlier, DIRECT hearing. Attributing those to the new packet rendered
   somebody on the far side of a gateway as "standing next to you", with
   the reading's age re-stamped to zero (measured, PR #306 review:
   `heardDirect` stayed `true` and `directSignal.ageMs` returned to 0
   after a `via_mqtt` packet). So `MeshRxMeta` also carries THIS
   packet's own hop path and THIS packet's own RSSI, and `CoreStore`
   attributes off those whenever a snapshot came from a packet at all —
   the rule `ff_shell.c`'s `shell_ev_rx_meta` has always applied on the
   puck (`m->rx_path == MC_RX_PATH_DIRECT && m->has_rssi`). AC13's "the
   four `crew.*` conditions are unchanged" is preserved in substance:
   the replay path (`rxMeta == nil`) keeps the pre-A02 rule exactly, and
   the packet path only ever REFUSES an attribution the old rule would
   have made. This also fixes the side effect that a live NodeInfo —
   which rebuilds a wrapper with no `hops_away` — otherwise cost a
   member their direct-signal attribution for the rest of the session.

8. **The app's hide list is not capped at `FF_HIDDEN_MAX` (16).** That
   bound is the puck's DRAM budget (S02's amendment §C); the phone
   stores hides as JSON per crew code and has no equivalent constraint,
   so it does not invent one. The puck's honest-failure copy at 16 is
   unaffected.

## 8. Open questions (only where a decision changes the work)

1. **Does `FF_CREW_MAX` stay 8?** Auto-membership makes 9+ a normal
   accident rather than a power-user edge. Raising it is a core change
   (static arrays, `check_dram_budget.py`, `rssi_hist[8][N]`) with real
   DRAM cost on the puck and none in the app, and §4.3's overflow
   handling is designed to be honest at 8 rather than to hide the
   limit. **Needs a number from the field test**, not an argument.
   Blocking nothing in slices A–E.
2. **Does the crew's human name need to converge across the crew?** §1.3
   accepts divergence (cosmetic, local). Making it converge needs a new
   `ff_proto` message and a "who wins" rule. Only worth it if the field
   test shows people confused about being in different crews when they
   are not.

Everything else the prompt raised is decided above: `info` is a constant
(§1.4), the channel name is the code (§1.3), region is never auto-set
(§1.7), the QR is the deep link with a Meshtastic fallback under
Advanced (§1.8), leaving restores the pre-crew channel (§3.4), and the
app's cap is the core's cap (§4.3).

---

## 9. Slices

Each slice is a PR. Bench = the two Heltec V3 boards
(`docs/hardware/heltec-v3.md`); loopback = the sim, mocks, and
`StubMeshtasticClient`.

### Slice A — codec + fixtures (`feat/a02a-crew-codec`)
- `CrewCode` (Swift, `FireflyModel`) and `ff_crewcode.c/h` (C, core):
  generate, parse/normalise, derive PSK, build `ChannelSettings`.
- `CrewInvite` deep-link encode/parse.
- `docs/specs/fixtures/A02-crew-codes.json` — the six vectors, consumed
  by both test suites.
- **Tests:** AC1–AC5. All loopback. C side under
  `firmware/core/tests/test_crewcode.c`; note the compiler in the PR
  body and run the GCC-14 second build (CLAUDE.md) — this slice is all
  string and byte handling, exactly the class `-Wstringop-truncation`
  catches.
- **Bench:** the §1.9 name-in-the-hash confirmation (two radios, same
  PSK, different names → cannot hear each other). Must land before
  slice B merges.
- HKDF: Swift uses CryptoKit's `HKDF<SHA256>`; C uses mbedTLS's
  `mbedtls_hkdf` on device and a vendored 40-line HMAC-SHA256 + expand
  in core for the sim/tests (core stays zero-dependency — this is the
  one place that rule costs us, and 40 lines with byte-exact vectors is
  the cheap way to pay it).

### Slice B — Start / Join / Crew page (`feat/a02b-start-join`)
- `CrewStartView`, `CrewJoinView`, `CrewScreen`, `CrewWelcome`.
- `CrewController` over the existing `preparePlan`/`confirmApply` path;
  the pre-crew snapshot; region gate; leave/change/rejoin.
- Crew-flavoured `AdminWriteConfirmationSheet` copy + Technical details
  disclosure.
- **Tests:** AC6–AC10, AC22. Loopback for everything except the write
  itself. **Bench required** for: a real `applyChannelSet` round trip
  (write → reboot → reconnect → read-back match), and the `UNSET` region
  path on a deliberately unconfigured board.
- Screenshots required (both simulators), per AGENTS.md and Jake's
  milestone-demo rule: welcome, start, join-scan, join-confirm, crew
  page.

### Slice C — auto-membership in the app (`feat/a02c-auto-crew`) `[api]`
- `MeshNodeSnapshot` gains a presence-flagged channel index and
  `via_mqtt`; `MeshtasticClient.handle(meshPacket:)` gains a
  `.nodeinfoApp` case. Both `[api]` — see §4.2.1, and do not start this
  slice without reading it.
- Crew-index resolution; the membership gate in front of `CoreStore`;
  the hide list; the overflow list and banner; migration.
- **Tests:** AC11–AC17. All loopback — `StubMeshtasticClient` can
  synthesize every packet shape, including `via_mqtt` and a 200-node
  replay. **Bench** for one end-to-end confirmation: board 2 joins,
  appears on board 1's phone with no taps, inside 60 s.
- This slice closes issue #266's live exposure (AC13); reference it.

### Slice D — puck firmware (`feat/a02d-puck-auto-crew`) `[api]`
Owned by `docs/specs/S02-core-crew.md`'s 2026-09-13 amendment; listed
here so the plan is one plan.
- `FF_CREW_AUTO_ON_CHANNEL` (default `y`) replacing
  `FF_DEV_TRUST_CHANNEL`; `mc_rx_meta_t` gains `channel_index` +
  `via_mqtt` `[api]`; meshclient surfaces the channel table `[api]`;
  `ff_hidden.h`; the CREW page's SHOW CODE face with `LV_USE_QRCODE`.
- **Tests:** S02_AC11–AC15 (amendment). Sim goldens for the code face.
  **Bench required** — this is the slice that cannot be believed without
  two radios and a puck.

### Slice E — copy and states (`feat/a02e-copy-states`)
- Permission strings; presence vocabulary; "Didn't get through"; the
  nameless fallback everywhere; the Advanced section; Connect reduced to
  a radio picker; the FIND-across-segments lifecycle fix.
- **Tests:** AC18–AC21, plus AC8's string test. All loopback.
- Can land in parallel with C; it touches copy and lifecycle, not the
  membership model.

**Order:** A → B → (C ∥ E) → D. D depends on A's codec only, so it can
start as soon as A merges and the bench confirmation is in.

## 10. Amendments

- **2026-09-14, bench finding — ask for a name on admission (the app
  half).** §4.4 says "when NodeInfo lands, the name updates in place"
  and left *when* that happens to the sending radio: a node auto-admitted
  (§4.1) on a Position/Text/`FF_PORTNUM` packet — anything but NodeInfo
  itself — reads **"New crew member"** until its OWN radio reaches its
  next periodic NodeInfo broadcast, which Meshtastic schedules on the
  order of hours. `docs/specs/S02-core-crew.md`'s 2026-09-14 amendment
  owns the puck's half of the fix and carries the full reasoning, the
  wire citations and the rate-limit rationale; **this amendment is the
  companion app's half**, and the two behave identically by design.

  - **The rule.** Immediately after `CrewMembershipEngine` admits a
    member (`admit(nodeID:)`'s `.paired` branch — never on a refusal,
    never on the `.full` overflow path), if that member has no display
    name ON THE ROSTER (`pairing.crew.member(nodeID:)`, never the
    packet that admitted them — an unhidden member's slot can already
    carry a name from before they were hidden), the app asks that node
    directly: `MeshtasticClientProtocol.requestNodeInfo(from:)`,
    fire-and-forget. §4.1 is untouched — this only ever FOLLOWS a
    successful admission and never causes one, so hidden / self /
    via-MQTT / wrong-channel / replay senders are all excluded by
    construction rather than by a second guard.
  - **The wire.** A `NODEINFO_APP` packet addressed to that node with
    `want_response = true`, `want_ack = false`, carrying **this node's
    own `User`** (the `ownerLongName`/`ownerShortName` the RADIO
    reported, never a guess, and absent rather than empty when the
    radio has reported none). The payload is not optional: a real
    `NodeInfoModule` hands whatever arrives to `NodeDB::updateUser`, so
    an empty `User` is a claim that this node has no name and blanks
    the peer's record of us — see `mc_send_nodeinfo_request`'s doc
    comment (`firmware/meshclient/include/mc_client.h`) for the
    verified firmware citation. Meshtastic's own iOS client sends its
    `User` on this exact request (`exchangeUserInfo`).
  - **Rate limit.** Once per node per ten minutes
    (`CrewNodeInfoRequestThrottle`, reset with the crew in
    `configure(crew:)`). An ordinary admission asks once by
    construction; the limit is the safety net for hide/unhide churn and
    roster-slot cycling, and it is recorded at the moment the decision
    is taken, not at send completion.
  - **The reply** needs no new seam: it arrives as an ordinary live
    `NODEINFO_APP` packet, is published on `nodeUpdates()` by the
    existing `.nodeinfoApp` decode case, and names the member through
    `CoreStore.apply(nodeUpdate:)` -> `crew.setIdentity`. Nothing about
    §4.4's display-name order or the "never write a fallback into the
    model" rule changes.
  - **Tests:** `CrewNodeInfoRequestOnAdmissionTests` (the rule, per
    clause), `CrewNodeInfoRequestThrottleTests` (the limit, pure),
    `ClientPositionAndPrivateTests` (the actual bytes on the wire:
    portnum, `want_response`, `want_ack`, and the `User` payload
    carrying the radio's own owner names).
