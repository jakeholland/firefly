# Licensing

**Firefly is GPL-3.0-only** (see [LICENSE](../LICENSE)). SPDX identifier: `GPL-3.0-only`.

## Why GPL, specifically

1. **Derivation:** `firmware/meshclient/proto/` contains C sources generated (via nanopb) from [meshtastic/protobufs](https://github.com/meshtastic/protobufs), which is GPL-3.0 licensed. Generated code is conservatively treated as derivative of the .proto definitions, and the firmware statically links it — so GPL-3.0 is the honest license for this codebase, not just a preference.
2. **Ecosystem:** Meshtastic's firmware and apps are GPL. Matching licenses keeps collaboration (and potential upstreaming) frictionless.
3. **Mission:** Firefly exists partly because its commercial predecessor was withdrawn from consumers. GPL is the license under which that cannot happen to this project.

## What GPL does and does not mean here

- **Selling kits and assembled pucks is fine.** GPL restricts hiding source, not commerce — Meshtastic hardware vendors operate commercially on the same terms. Ship the source (or a link here) with the product; done.
- Contributions are accepted under GPL-3.0-only (inbound = outbound; no CLA).
- The sub-libraries (`core/geo`, `core/t9`, `meshclient`, `festpack`) are GPL as part of this lineage. If a permissively-licensed standalone version of one is ever needed, it requires a clean-room implementation — do not copy from this tree into an MIT project.

## Related repos

- [fest-almanac](https://github.com/jakeholland/fest-almanac): pack **data** is CC0-1.0 (public domain — any app may consume it, no strings); **schema and tooling** are MIT. Deliberately permissive so the festival-data commons outlives any one consumer, including this one.

## TODO

- [ ] SPDX headers (`// SPDX-License-Identifier: GPL-3.0-only`) on source files — add per-module as files are next touched, or in one sweep PR.
