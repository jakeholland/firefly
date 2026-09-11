//
//  ChannelImportViewModel.swift — the Connect screen's channel-import
//  section: paste or scan a `https://meshtastic.org/e/#…` link, show
//  what it decodes to (docs/specs/A01-companion-app.md's M1
//  Connect-screen bullet).
//
//  M3 update: write-back has landed (A01 M3: "Channel write-back
//  (admin messages) behind an explicit confirmation"). `importURL`/
//  `clear`/`precisionWarnings` are exactly M1's — parsing a link still
//  never writes anything on its own. The only new surface is
//  `applySummary` (what a confirmation sheet shows) and `confirmApply()`
//  (the ONE method that actually calls `client.applyChannelSet`, and
//  the only thing that ever does).
//
import FireflyMesh
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class ChannelImportViewModel {
    private(set) var result: ChannelImportResult?
    private(set) var errorMessage: String?
    private let client: any MeshtasticClientProtocol

    /// M3 — confirm-then-write state. `isApplying` gates the
    /// confirmation sheet's CONFIRM button so a second tap cannot start
    /// a second concurrent write; `applyErrorMessage` is cleared on
    /// every new import AND on every new apply attempt, never left
    /// stale from a previous one.
    private(set) var isApplying = false
    private(set) var applyErrorMessage: String?
    private(set) var lastAppliedReport: ChannelWriteReport?

    /// Defaulted so every existing call site (`ChannelImportViewModel()`
    /// in tests predating M3) keeps compiling — the same
    /// "appended-with-a-default, nothing breaks" convention
    /// `AppDependencies.swift`'s own `scanner`/`crewPairingStore`
    /// fields use.
    init(client: any MeshtasticClientProtocol = StubMeshtasticClient()) {
        self.client = client
    }

    func importURL(_ text: String) {
        errorMessage = nil
        applyErrorMessage = nil
        lastAppliedReport = nil
        do {
            result = try ChannelURL.parse(text)
        } catch {
            result = nil
            errorMessage = Self.message(for: error)
        }
    }

    func clear() {
        result = nil
        errorMessage = nil
        applyErrorMessage = nil
        lastAppliedReport = nil
    }

    /// The one thing this screen must never say about a channel it
    /// just imported: that its position precision is safe when the URL
    /// never said so. Surfaced explicitly rather than silently patched
    /// — see `ChannelSettings.missingExplicitPositionPrecision`.
    var precisionWarnings: [String] {
        guard let result else { return [] }
        return result.channelSet.settings
            .filter(\.missingExplicitPositionPrecision)
            .map { settings in
                let name = settings.name.isEmpty ? "(default)" : settings.name
                return "\(name): no position-precision limit was in this link — a node that " +
                       "later sends this channel treats that as FULL precision, exact coordinates."
            }
    }

    /// Everything the "Apply to node" confirmation sheet shows, computed
    /// straight from `result` — never re-derived after a write, so the
    /// sheet can never describe anything other than the change
    /// `confirmApply()` is about to make.
    var applySummary: ChannelApplySummary? {
        guard let result else { return nil }
        return ChannelApplySummary(result: result)
    }

    /// Behind explicit confirmation only — the Connect screen's
    /// confirmation sheet is the ONE caller. Sends `result`'s write
    /// request to the connected node and reports honest success (the
    /// node's own read-back matched) or a clear error.
    @discardableResult
    func confirmApply() async -> Bool {
        guard let result else { return false }
        isApplying = true
        applyErrorMessage = nil
        defer { isApplying = false }
        do {
            let report = try await client.applyChannelSet(result.makeChannelWriteRequest())
            lastAppliedReport = report
            return true
        } catch {
            applyErrorMessage = Self.writeMessage(for: error)
            return false
        }
    }

    private static func message(for error: Error) -> String {
        guard let channelError = error as? ChannelURLError else { return String(describing: error) }
        switch channelError {
        case .unsupportedScheme: return "Not a Meshtastic channel link."
        case .missingFragment: return "The link has no channel payload."
        case .malformedBase64: return "The channel payload isn't valid."
        case .malformedProtobuf: return "The channel payload couldn't be read."
        }
    }

    static func writeMessage(for error: Error) -> String {
        guard let writeError = error as? AdminWriteError else { return String(describing: error) }
        switch writeError {
        case .notConnected: return "Not connected to a node."
        case .encodingFailed: return "Could not build the admin message."
        case .timeout: return "The node did not answer in time — it may still be rebooting."
        case .readBackMismatch(let detail): return "The node did not confirm the change: \(detail)"
        }
    }
}

/// Exactly what the "Apply to node" confirmation sheet shows — computed
/// once, up front, from the SAME `ChannelImportResult` `confirmApply()`
/// sends, never re-derived afterward.
struct ChannelApplySummary {
    /// One line per channel, e.g. "Firefly (index 0, primary) — full precision".
    let channelLines: [String]
    /// nil when this import carried no LoRa config — write-back leaves
    /// the node's region/modem preset untouched in that case.
    let regionLine: String?

    init(result: ChannelImportResult) {
        channelLines = result.channelSet.settings.enumerated().map { offset, settings in
            let name = settings.name.isEmpty ? "(default channel)" : settings.name
            let role = offset == 0 ? "primary" : "secondary"
            let precision: String
            if settings.hasModuleSettings {
                precision = "precision \(settings.moduleSettings.positionPrecision) bits"
            } else {
                precision = "no precision limit in this link — will write FULL precision"
            }
            return "\(name) (index \(offset), \(role)) — \(precision)"
        }
        if result.channelSet.hasLoraConfig {
            let lora = result.channelSet.loraConfig
            let preset = lora.usePreset ? String(describing: lora.modemPreset) : "custom"
            regionLine = "Region \(String(describing: lora.region).uppercased()), modem preset \(preset)"
        } else {
            regionLine = nil
        }
    }
}
