//
//  CrewController.swift — Slice B's one view model behind Start a crew /
//  Join a crew / the Crew page (`docs/specs/A02-crew-join.md`, §2, §3,
//  §5). Owns the plain-language state those screens render; every
//  actual radio write goes through the EXISTING `ChannelImportViewModel`
//  `preparePlan()`/`confirmApply()` path (§2.1 step 5, §3.3 step 5) —
//  this file mints/derives the channel and builds the plain-language
//  confirmation copy, it never talks to `MeshtasticClientProtocol`
//  directly for a write.
//
//  MVVM shape: `@MainActor @Observable`, holds protocols
//  (`MeshtasticClientProtocol`, `CrewProfileStoring`,
//  `CrewSnapshotStoring`, `CrewHiddenStoring`), no I/O of its own beyond
//  what `ChannelImportViewModel`/`client` already do.
//
import FireflyMesh
import FireflyModel
import Foundation
import MeshtasticProto
import Observation

@MainActor
@Observable
final class CrewController {
    // MARK: - Crew identity

    /// The crew this phone is currently on — `nil` before any Start/Join
    /// (or after Leave). Loaded from `profileStore` at init.
    private(set) var profile: CrewProfile?
    var hasCrew: Bool { profile != nil }

    // MARK: - Region gate (§1.7)

    /// `true` only when the connected radio has EXPLICITLY reported
    /// `.unset` — never when the region is simply not yet known
    /// (`connectedNodeConfig` is `nil`, or its `region` is `nil`), which
    /// is not the same fact and must not block a join that just hasn't
    /// heard `want_config`'s `Config` frame yet.
    ///
    /// `didConfirmRegionThisSession` is consulted FIRST: a real
    /// `MeshtasticClientProtocol` re-reads `nodeConfig` off the radio's
    /// own read-back after `setRegion` succeeds, but nothing in this
    /// file's contract requires that of every conformance (in
    /// particular, `StubMeshtasticClient` — used by every test and by
    /// `.stub()` — records the write and nothing else). Without this
    /// flag, a successful `confirmRegion()` against such a client would
    /// leave `regionIsUnset` reporting `true` forever and this screen
    /// stuck on the gate despite having just done exactly what the gate
    /// asked.
    var regionIsUnset: Bool {
        !didConfirmRegionThisSession && client.connectedNodeConfig?.region == .unset
    }
    private var didConfirmRegionThisSession = false
    /// A default in a CONTROL, never an applied write (§1.7) —
    /// `confirmRegion()` is the one place `setRegion` is ever called
    /// from this file, and only once the user taps SAVE.
    var regionSelection: Config.LoRaConfig.RegionCode
    private(set) var regionErrorMessage: String?
    private(set) var isSettingRegion = false

    /// §1.7: "The picker is prefilled from `Locale.current.region`."
    static func suggestedRegion(for locale: Locale = .current) -> Config.LoRaConfig.RegionCode {
        guard let identifier = locale.region?.identifier.uppercased() else { return .us }
        switch identifier {
        case "US", "CA", "MX", "PR": return .us
        case "AU", "NZ": return .anz
        case "JP": return .jp
        case "KR": return .kr
        case "TW": return .tw
        case "CN": return .cn
        case "RU": return .ru
        default: return eu868Locales.contains(identifier) ? .eu868 : .us
        }
    }
    /// Not exhaustive — this is a CONFIRMABLE suggestion, not a silent
    /// write (§1.7); an unrecognised locale falls back to `.us` and the
    /// user picks the right one themselves.
    private static let eu868Locales: Set<String> = [
        "GB", "IE", "FR", "DE", "ES", "IT", "NL", "BE", "PT", "AT",
        "CH", "SE", "NO", "DK", "FI", "PL", "GR", "CZ", "HU", "RO",
    ]

    /// AC7: `setRegion(.unset)` is never called — guarded here, not just
    /// trusted to the caller disabling the button.
    @discardableResult
    func confirmRegion() async -> Bool {
        guard regionSelection != .unset else { return false }
        isSettingRegion = true
        regionErrorMessage = nil
        defer { isSettingRegion = false }
        do {
            _ = try await client.setRegion(regionSelection)
            didConfirmRegionThisSession = true
            return true
        } catch {
            regionErrorMessage = ChannelImportViewModel.writeMessage(for: error)
            return false
        }
    }

    // MARK: - Dependencies

    let importer: ChannelImportViewModel
    private let client: any MeshtasticClientProtocol
    private let profileStore: any CrewProfileStoring
    private let snapshotStore: any CrewSnapshotStoring
    let hiddenStore: any CrewHiddenStoring
    /// Wall-clock epoch milliseconds — deliberately NOT
    /// `FireflyClock.nowMillis()` (`ff_crew`'s 32-bit-truncated, roughly
    /// 49-day-wraparound convention, right for a RAM-only session clock,
    /// wrong for a value this file PERSISTS as `CrewProfile.createdAtMs`
    /// and expects to still mean something after a relaunch days later).
    private let clock: () -> UInt64

    init(client: any MeshtasticClientProtocol,
         importer: ChannelImportViewModel? = nil,
         profileStore: any CrewProfileStoring = InMemoryCrewProfileStore(),
         snapshotStore: any CrewSnapshotStoring = InMemoryCrewSnapshotStore(),
         hiddenStore: any CrewHiddenStoring = InMemoryCrewHiddenStore(),
         clock: @escaping () -> UInt64 = { UInt64((Date().timeIntervalSince1970 * 1000).rounded()) }) {
        self.client = client
        self.importer = importer ?? ChannelImportViewModel(client: client)
        self.profileStore = profileStore
        self.snapshotStore = snapshotStore
        self.hiddenStore = hiddenStore
        self.clock = clock
        self.profile = profileStore.load()
        self.regionSelection = Self.suggestedRegion()
    }

    // MARK: - Shared prepare/confirm state (Start AND Join both use this)

    /// What is currently staged for the confirmation sheet — mutually
    /// exclusive with everything else in this section. `nil` once
    /// `confirmApply()`/`cancelPending()` runs.
    enum PendingKind: Equatable {
        case start(code: CrewCode, humanName: String)
        case join(code: CrewCode, name: String?, changingFrom: String?)
    }
    private(set) var pending: PendingKind?
    private(set) var isPreparing = false
    private(set) var isApplying = false
    private(set) var errorMessage: String?
    /// Set instead of staging a join — §3.4: scanning your own code is a
    /// no-op with a friendly confirmation, never a write.
    private(set) var rejoinOwnCrewMessage: String?

    var isBusy: Bool { isPreparing || isApplying }

    /// §2.1: mint a fresh code and stage it for confirmation.
    @discardableResult
    func beginStart(humanName: String) async -> Bool {
        errorMessage = nil
        rejoinOwnCrewMessage = nil
        let code = CrewCode.generate()
        let trimmed = humanName.trimmingCharacters(in: .whitespacesAndNewlines)
        let name = trimmed.isEmpty ? "My crew" : trimmed
        guard await preparePlan(for: code) else { return false }
        pending = .start(code: code, humanName: name)
        return true
    }

    /// §3.1/§3.3: stage a scanned/typed code for confirmation. Handles
    /// the "already in this crew" (§3.4) and "not a crew code at all"
    /// cases without ever calling `preparePlan()`.
    @discardableResult
    func beginJoin(payload: CrewScanPayload) async -> Bool {
        switch payload {
        case .crewLink(let link):
            return await beginJoin(code: link.code, name: link.name)
        case .bareCode(let code):
            return await beginJoin(code: code, name: nil)
        case .meshtasticChannelLink, .unrecognized:
            return false
        }
    }

    private func beginJoin(code: CrewCode, name: String?) async -> Bool {
        errorMessage = nil
        rejoinOwnCrewMessage = nil
        if let profile, profile.code == code.canonical {
            rejoinOwnCrewMessage = "You're already in \(profile.humanName)."
            return false
        }
        let changingFrom = profile?.humanName
        guard await preparePlan(for: code) else { return false }
        pending = .join(code: code, name: name, changingFrom: changingFrom)
        return true
    }

    private func preparePlan(for code: CrewCode) async -> Bool {
        isPreparing = true
        defer { isPreparing = false }
        await snapshotCurrentPrimaryIfNeeded()
        let url = ChannelURL.encode(CrewChannel.channelSet(for: code), addMode: false)
        importer.importURL(url)
        let ok = await importer.preparePlan()
        if !ok { errorMessage = importer.planErrorMessage ?? "Couldn't prepare that crew." }
        return ok
    }

    func cancelPending() {
        pending = nil
        importer.clear()
        errorMessage = nil
    }

    // MARK: - Plain-language confirmation copy (§2.2, §3.3, AC8)

    /// AC8: contains none of "node", "channel", "index", "precision",
    /// "preset", "region", "PSK", "Meshtastic".
    var confirmationTitle: String {
        switch pending {
        case .start(_, let name): return "Start \(name)?"
        case .join(_, _, .some): return "Join a new crew?"
        case .join(_, let name, nil): return "Join \(name ?? "this crew")?"
        case nil: return ""
        }
    }

    /// The ONE sentence above the fold (`AdminWriteConfirmationSheet
    /// .primaryText`) — what this write does to the person's crew, plus
    /// the shared `AdminWriteCopy.radioBlinksOff` sentence every other
    /// admin write on this sheet already ends with. §2.2's own layout:
    /// plain sentence first, technical lines behind the disclosure.
    var confirmationPrimaryText: String {
        switch pending {
        case .start:
            return "This puts your puck on a private crew that only people with your code can see. " +
                "Your crew will see exactly where you are. \(AdminWriteCopy.radioBlinksOff)"
        case .join:
            return "Only people with this code can see this crew. Your crew will see exactly " +
                "where you are. \(AdminWriteCopy.radioBlinksOff)"
        case nil:
            return ""
        }
    }

    /// The extra plain line a CHANGE-of-crew adds (§3.4) — and nothing
    /// else. Start and a first-ever Join say everything they need to in
    /// `confirmationPrimaryText`; repeating it here would print it
    /// twice on the sheet.
    var confirmationLines: [String] {
        guard case .join(_, let name, .some(let previous)) = pending else { return [] }
        let target = name ?? "the new crew"
        return ["You'll leave \(previous) and join \(target). You can come back with \(previous)'s code."]
    }

    /// The existing `ChannelApplySummary` lines, verbatim, behind
    /// "Technical details ›" (§2.2).
    var confirmationTechnicalDetails: [String] {
        guard let summary = importer.applySummary else { return [] }
        var lines = summary.channelLines + summary.disabledLines + summary.untouchedLines
        if let regionLine = summary.regionLine { lines.append(regionLine) }
        return lines
    }

    /// Confirms whichever of Start/Join is staged — the ONE place this
    /// file calls `importer.confirmApply()`.
    @discardableResult
    func confirmApply() async -> Bool {
        guard let pending else { return false }
        isApplying = true
        defer { isApplying = false }
        let ok = await importer.confirmApply()
        guard ok else {
            errorMessage = importer.applyErrorMessage ?? "Couldn't apply that crew."
            return false
        }
        switch pending {
        case .start(let code, let name):
            adoptProfile(code: code, humanName: name)
        case .join(let code, let name, _):
            adoptProfile(code: code, humanName: name ?? code.canonical)
        }
        self.pending = nil
        return true
    }

    private func adoptProfile(code: CrewCode, humanName: String) {
        if let old = profile {
            profileStore.rememberRecentCrew(RecentCrew(code: old.code, humanName: old.humanName))
        }
        let new = CrewProfile(code: code.canonical, humanName: humanName, createdAtMs: clock())
        profileStore.save(new)
        profile = new
    }

    // MARK: - Leave (§3.4)

    private(set) var leaveErrorMessage: String?

    /// §3.4: writes the radio back — the pre-crew snapshot if this phone
    /// has one, else the stock default primary (`position_precision ==
    /// 0`). Never a local-only forget.
    @discardableResult
    func leaveCrew() async -> Bool {
        guard let profile else { return false }
        isApplying = true
        leaveErrorMessage = nil
        defer { isApplying = false }

        let restore = snapshotStore.load() ?? .stockDefault
        var settings = ChannelSettings()
        settings.name = restore.name
        settings.psk = restore.psk
        settings.moduleSettings.positionPrecision = restore.positionPrecision
        let channelSet = ChannelSet(settings: [settings], loraConfig: nil)
        let url = ChannelURL.encode(channelSet, addMode: false)

        importer.importURL(url)
        guard await importer.preparePlan() else {
            leaveErrorMessage = importer.planErrorMessage ?? "Couldn't prepare leaving this crew."
            return false
        }
        guard await importer.confirmApply() else {
            leaveErrorMessage = importer.applyErrorMessage ?? "Couldn't leave this crew."
            return false
        }
        profileStore.rememberRecentCrew(RecentCrew(code: profile.code, humanName: profile.humanName))
        profileStore.clear()
        self.profile = nil
        return true
    }

    // MARK: - Hidden set (§4.5)

    func hiddenIDs() -> Set<UInt32> {
        guard let profile else { return [] }
        return hiddenStore.hiddenIDs(forCrew: profile.code)
    }

    /// §4.5: hide = unpair in `ff_crew` + persist the id on the hide
    /// list. `pairing` is passed in rather than held, so this file does
    /// not need its own `CrewPairingController` reference beyond what
    /// each call actually needs.
    func hide(nodeID: UInt32, pairing: CrewPairingController) {
        guard let profile else { return }
        var ids = hiddenStore.hiddenIDs(forCrew: profile.code)
        ids.insert(nodeID)
        hiddenStore.setHiddenIDs(ids, forCrew: profile.code)
        pairing.unpair(nodeID: nodeID)
    }

    func unhide(nodeID: UInt32) {
        guard let profile else { return }
        var ids = hiddenStore.hiddenIDs(forCrew: profile.code)
        ids.remove(nodeID)
        hiddenStore.setHiddenIDs(ids, forCrew: profile.code)
    }

    func rename(humanName: String) {
        guard var profile = self.profile else { return }
        profile.humanName = humanName
        profileStore.save(profile)
        self.profile = profile
    }

    // MARK: - Pre-crew snapshot (§2.1 step 4)

    private func snapshotCurrentPrimaryIfNeeded() async {
        guard snapshotStore.load() == nil else { return }
        guard let table = try? await client.currentChannelTable(),
              let primary = table.first(where: { $0.index == 0 }) else { return }
        let settings = primary.settings
        // §2.1 step 4's "never overwritten by a Firefly crew channel" is
        // a rule about what may be CAPTURED, not only about capturing
        // once (PR #308 review). "Once only" alone is not enough: a
        // reinstall onto a radio this app already moved onto a crew
        // starts with an empty snapshot store and a primary named
        // `FIRE-XXXXXX`, and would latch THAT as the pre-crew state —
        // so Leave would then "restore" the user onto a crew channel,
        // still transmitting precise positions, which is exactly the
        // outcome §3.4 calls the worst possible one. A channel whose
        // name parses as a crew code is never a pre-crew channel;
        // skipping it leaves `Leave` on `.stockDefault` (precision 0),
        // which is the honest answer for a phone that has no record of
        // what came before.
        guard (try? CrewCode.parse(settings.name)) == nil else { return }
        snapshotStore.saveIfAbsent(CrewPreCrewSnapshot(
            name: settings.name,
            psk: settings.psk,
            positionPrecision: settings.hasModuleSettings ? settings.moduleSettings.positionPrecision : 0))
    }
}
