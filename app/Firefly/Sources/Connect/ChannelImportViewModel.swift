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

    /// M3 (PR #274 review, BLOCKING 1 & 2) — the plan `preparePlan()`
    /// builds against the node's LIVE channel occupancy, BEFORE the
    /// confirmation sheet ever shows. `confirmApply()` sends exactly
    /// `applyPlan.request` and nothing else, so the sheet (built from
    /// this same plan, see `applySummary`) can never describe a
    /// different write than the one that actually goes out.
    private(set) var applyPlan: ChannelWritePlan?
    /// Set when `preparePlan()` cannot honestly build a plan — most
    /// commonly an "add" import with no free secondary slot. Distinct
    /// from `applyErrorMessage` (a WRITE that was attempted and failed):
    /// this is a plan that was never attempted because it cannot be done
    /// safely, so nothing about a confirmation sheet should show at all.
    private(set) var planErrorMessage: String?
    private(set) var isPreparingPlan = false

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
        applyPlan = nil
        planErrorMessage = nil
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
        applyPlan = nil
        planErrorMessage = nil
    }

    /// M3 (PR #274 review, BLOCKING 1 & 2) — the ONE place
    /// `ChannelImportResult.makeChannelWritePlan(occupiedIndexes:)` is
    /// called. Reads the node's CURRENT channel table live
    /// (`client.currentChannelTable()`) so an "add" import's free-slot
    /// placement reflects what is actually on the radio right now, never
    /// a stale guess — then builds the plan the confirmation sheet shows
    /// AND `confirmApply()` sends. Must be called (and must succeed)
    /// BEFORE the confirmation sheet is shown; the Connect screen's
    /// "APPLY TO NODE" button calls this first and only opens the sheet
    /// once `applyPlan` is non-nil.
    @discardableResult
    func preparePlan() async -> Bool {
        guard let result else { return false }
        isPreparingPlan = true
        planErrorMessage = nil
        applyPlan = nil
        applyErrorMessage = nil
        defer { isPreparingPlan = false }
        do {
            let occupied = try await currentOccupiedIndexes()
            applyPlan = try result.makeChannelWritePlan(occupiedIndexes: occupied)
            return true
        } catch {
            planErrorMessage = Self.planMessage(for: error)
            return false
        }
    }

    private func currentOccupiedIndexes() async throws -> Set<Int32> {
        let table = try await client.currentChannelTable()
        return Set(table.map(\.index))
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
    /// straight from `applyPlan` — never from `result` directly, and
    /// never re-derived after a write — so the sheet can never describe
    /// anything other than the exact write `confirmApply()` is about to
    /// send (PR #274 review, BLOCKING 1 & 2: which slots are written,
    /// which are disabled, which are untouched).
    var applySummary: ChannelApplySummary? {
        guard let applyPlan else { return nil }
        return ChannelApplySummary(plan: applyPlan)
    }

    /// Behind explicit confirmation only — the Connect screen's
    /// confirmation sheet is the ONE caller, and only once `preparePlan()`
    /// has already produced `applyPlan`. Sends `applyPlan.request` —
    /// EXACTLY what `applySummary` just described — to the connected
    /// node and reports honest success (the node's own read-back
    /// matched) or a clear error.
    @discardableResult
    func confirmApply() async -> Bool {
        guard let applyPlan else { return false }
        isApplying = true
        applyErrorMessage = nil
        defer { isApplying = false }
        do {
            let report = try await client.applyChannelSet(applyPlan.request)
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

    /// PR #274 review, BLOCKING 1 — messages for `ChannelWritePlanError`,
    /// surfaced by `preparePlan()` before any write is attempted.
    private static func planMessage(for error: Error) -> String {
        guard let planError = error as? ChannelWritePlanError else { return String(describing: error) }
        switch planError {
        case .noFreeChannelSlots:
            return "No free channel slots — remove a secondary channel on the node first."
        case .notEnoughFreeChannelSlots(let needed, let available):
            return "Not enough free channel slots: this link needs \(needed), the node has \(available) free."
        case .tooManyChannels(let count):
            return "This link has \(count) channels — a Meshtastic radio supports up to 8."
        }
    }

    static func writeMessage(for error: Error) -> String {
        guard let writeError = error as? AdminWriteError else { return String(describing: error) }
        switch writeError {
        case .notConnected: return "Not connected to a node."
        case .encodingFailed: return "Could not build the admin message."
        case .timeout: return "The node did not answer in time — it may still be rebooting."
        case .readBackMismatch(let detail): return "The node did not confirm the change: \(detail)"
        case .regionUnset: return "UNSET is not a region to apply — pick one first."
        case .partialApplyFailed(let step, let underlying):
            return "Failed sending \(step): \(underlying). The node may be partially configured — " +
                   "reconnect and check its channels before trying again."
        }
    }
}

/// Exactly what the "Apply to node" confirmation sheet shows — computed
/// once, up front, from the SAME `ChannelWritePlan` `confirmApply()`
/// sends, never re-derived afterward. States all three fates a slot can
/// have (PR #274 review, BLOCKING 2): WRITTEN, DISABLED, or UNTOUCHED.
struct ChannelApplySummary {
    /// One line per channel actually written, e.g.
    /// "Firefly (index 0, primary) — precision 32 bits".
    let channelLines: [String]
    /// One line per slot this plan explicitly disables — empty for an
    /// "add" plan.
    let disabledLines: [String]
    /// One line naming every slot this plan neither writes nor disables
    /// — empty for a "replace" plan (it accounts for every slot).
    let untouchedLines: [String]
    /// nil when this import carried no LoRa config — write-back leaves
    /// the node's region/modem preset untouched in that case.
    let regionLine: String?
    let addMode: Bool

    init(plan: ChannelWritePlan) {
        addMode = plan.addMode
        channelLines = plan.writtenChannels.map { entry in
            let role = entry.isPrimary ? "primary" : "secondary"
            let precision: String
            if entry.precisionWasExplicit {
                precision = "precision \(entry.positionPrecisionBits) bits"
            } else {
                precision = "no precision limit in this link — will write the SAFE default " +
                            "(\(entry.positionPrecisionBits), don't share)"
            }
            return "\(entry.name) (index \(entry.index), \(role)) — \(precision)"
        }
        disabledLines = plan.disabledIndexes.map { "index \($0) — DISABLED" }
        untouchedLines = plan.untouchedIndexes.map { "index \($0) — untouched" }
        if let lora = plan.request.loraConfig {
            let preset = lora.usePreset ? String(describing: lora.modemPreset) : "custom"
            regionLine = "Region \(String(describing: lora.region).uppercased()), modem preset \(preset)"
        } else {
            regionLine = nil
        }
    }
}
