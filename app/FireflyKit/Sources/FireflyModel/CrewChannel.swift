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
//    lora_config          never set — §1.7, region/preset untouched
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
    /// `loraConfig` is never set — §1.7: a crew join never touches
    /// region or modem preset.
    public static func channelSet(for code: CrewCode) -> ChannelSet {
        ChannelSet(settings: [channelSettings(for: code)], loraConfig: nil)
    }

    /// §1.8's "Copy Meshtastic link" — a REPLACE url (no `?add=true`),
    /// built by the existing `ChannelURL.encode`, so any client that
    /// understands a plain Meshtastic channel link (CLI, stock app, a
    /// puck provisioned by hand) can import this exact crew.
    public static func meshtasticURL(for code: CrewCode) -> String {
        ChannelURL.encode(channelSet(for: code), addMode: false)
    }
}
