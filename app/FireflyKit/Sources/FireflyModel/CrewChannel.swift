//
//  CrewChannel.swift — the exact `ChannelSettings`/`ChannelSet` a crew
//  code derives, and the `https://meshtastic.org/e/#…` fallback URL
//  built from it (`docs/specs/A02-crew-join.md`, §1.5, §1.8).
//
//  Every field here is pinned byte-for-byte by
//  `docs/specs/fixtures/A02-crew-codes.json` (§1.9) — nothing in this
//  file is "reasonable defaults", it is the spec's own table:
//
//    psk                  the 32 derived bytes (CrewKey.psk)
//    name                 the canonical code, 11 chars
//    channel_num, id      unset (proto3 default — never set explicitly)
//    uplink/downlinkEnabled  false (proto3 default — never bridge to MQTT)
//    module_settings.position_precision  32, ALWAYS explicitly present
//    lora_config          never set on `channelSet(for:)` — §1.7, a crew
//                         JOIN never touches region or modem preset.
//
//  §1.4/§1.8 AMENDMENT, 2026-09-14 (bench finding): `channelSet(for:)`
//  above is what Firefly's OWN join path writes via admin `set_channel`
//  — unaffected. But the SEPARATE "Copy Meshtastic link" export
//  (`exportChannelSet`/`meshtasticURL` below) is consumed by OTHER
//  tools: the Meshtastic Python CLI's `--seturl` and the official apps'
//  URL import REPLACE the target radio's entire `lora_config` from the
//  URL. An absent `lora_config` therefore does not mean "leave it
//  alone" to them — it means "write an EMPTY one": `use_preset = false`,
//  `region = UNSET`. Measured on a Heltec V3: importing the old
//  no-lora-config URL left the radio deaf (`"region": "UNSET",
//  "usePreset": false`); restoring a `--qr`-style URL that carries
//  `lora_config` brought it back. So the export path now copies the
//  CONNECTED radio's CURRENT `Config.LoRaConfig` into the exported
//  `ChannelSet` — never a guess, always exactly what the radio itself
//  is doing right now.
//
import FireflyMesh
import Foundation
import MeshtasticProto

public enum CrewChannel {
    /// §1.5's table, built once from a code. `moduleSettings
    /// .positionPrecision = 32` is set explicitly (not left at the
    /// proto3 default of 0) precisely so `hasModuleSettings` is `true`
    /// — the join confirmation's "Your crew will see exactly where you
    /// are" sentence is always true of what actually gets written, and
    /// `ChannelURL.withExplicitPositionPrecision`'s "absent means 32 on
    /// a write anyway" trap never has anything to silently paper over
    /// here.
    public static func channelSettings(for code: CrewCode) -> ChannelSettings {
        var settings = ChannelSettings()
        settings.psk = CrewKey.psk(for: code)
        settings.name = code.canonical
        settings.moduleSettings.positionPrecision = 32
        settings.uplinkEnabled = false
        settings.downlinkEnabled = false
        return settings
    }

    /// One-entry `ChannelSet`, index 0 / primary — §1.5's `Channel.index
    /// = 0`, `Channel.role = PRIMARY` live in `ChannelImportResult
    /// .makeChannelWritePlan(occupiedIndexes:)`'s existing "replace"
    /// path (every crew join reuses that path unchanged, per §2.1/§3.3).
    /// `loraConfig` is never set here — §1.7: a crew join never touches
    /// region or modem preset. **Not** what "Copy Meshtastic link" sends
    /// any more — see `exportChannelSet(for:loraConfig:)` below.
    public static func channelSet(for code: CrewCode) -> ChannelSet {
        ChannelSet(settings: [channelSettings(for: code)], loraConfig: nil)
    }

    /// Thrown by `exportChannelSet`/`meshtasticURL` when asked to export
    /// with a `lora_config` whose region is `.unset` — Meshtastic's own
    /// "radio disabled" sentinel (same defense-in-depth stance as
    /// `MeshtasticClient.setRegion`'s identical guard). Exporting it
    /// anyway would hand the importing radio a link that, per the bench
    /// finding above, writes it deaf; refusing here means the caller
    /// (`CrewController`) never has a code path that can produce that
    /// URL, not just a UI that discourages it.
    public enum ExportError: Error, Equatable, Sendable {
        case regionUnset
    }

    /// The `ChannelSet` "Copy Meshtastic link" actually encodes: the
    /// crew's own channel (unchanged from `channelSet(for:)`) PLUS the
    /// connected radio's CURRENT `lora_config`, copied verbatim — never
    /// synthesised, never defaulted. `region == .unset` throws rather
    /// than exporting a link that would blank the importing radio's
    /// region (§1.8 amendment).
    public static func exportChannelSet(for code: CrewCode, loraConfig: Config.LoRaConfig) throws -> ChannelSet {
        guard loraConfig.region != .unset else { throw ExportError.regionUnset }
        return ChannelSet(settings: [channelSettings(for: code)], loraConfig: loraConfig)
    }

    /// §1.8's "Copy Meshtastic link" — a REPLACE url (no `?add=true`),
    /// built by the existing `ChannelURL.encode`, so any client that
    /// understands a plain Meshtastic channel link (CLI, stock app, a
    /// puck provisioned by hand) can import this exact crew — WITHOUT
    /// also importing an empty `lora_config` that would go deaf on
    /// arrival (§1.8 amendment, 2026-09-14). `loraConfig` must be the
    /// CONNECTED radio's own current LoRa config
    /// (`MeshtasticClientProtocol.connectedNodeConfig?.loraConfig`) —
    /// this function never invents one.
    public static func meshtasticURL(for code: CrewCode, loraConfig: Config.LoRaConfig) throws -> String {
        ChannelURL.encode(try exportChannelSet(for: code, loraConfig: loraConfig), addMode: false)
    }
}
