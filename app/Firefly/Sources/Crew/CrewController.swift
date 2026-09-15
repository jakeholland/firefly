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

    /// Called whenever `profile` CHANGES — a Start, a Join, a "Start a
    /// new crew" switch (§6.5), or a Leave. `FireflyApp` points this at
    /// `AppGraph.syncCrewMembershipWithProfile()`, which is how
    /// `CrewMembershipEngine` learns which crew to admit for; before PR
    /// #313 nothing did, and auto-membership was inert in the shipped
    /// app.
    ///
    /// A plain closure rather than a `CrewMembershipEngine` reference:
    /// this controller has no business knowing the engine exists, and
    /// every test in `CrewControllerTests` composes it without a graph.
    /// Not called by `rename(humanName:)` — a rename changes the human
    /// label, never the code, and the engine keys off the code.
    var onProfileChanged: (@MainActor () -> Void)?
    var hasCrew: Bool { profile != nil }

    // MARK: - Radio gate (A02 §2.1 step 1 / §3.3 step 1)
    //
    // Owner report, 2026-09-14, build 328 on the iPhone: "Tried to join
    // but nothing happened, still on the Join a crew screen."
    //
    // Root cause, traced end to end: JOIN -> `beginJoin(payload:)` ->
    // `beginJoin(code:name:)`'s `guard await preparePlan(for: code) else
    // { return false }` -> `preparePlan(for:)`'s `importer.preparePlan()`
    // -> `ChannelImportViewModel.currentOccupiedIndexes()` ->
    // `client.currentChannelTable()`, which throws
    // `AdminWriteError.notConnected` the moment there is no radio
    // (`MeshtasticClient.requireConnectedNode()`). That error was not an
    // `ChannelWritePlanError`, so `ChannelImportViewModel.planMessage(for:)`
    // fell through to `String(describing:)` and the whole failure reached
    // the screen as the single word `notConnected`, in a `.footnote`
    // `Color.ffAlert` line under a JOIN button that stayed enabled — a
    // silent no-op in every way that matters to the person holding the
    // phone.
    //
    // The fix is two-sided: the message is plain language now
    // (`planMessage(for:)`), AND the attempt is refused up front, here,
    // with a state the screens render as a banner instead of letting a
    // doomed write start.

    /// This controller's own `@Observable` mirror of `client.linkState()`.
    ///
    /// It exists for SwiftUI's benefit, not for the gate's: the ANSWER
    /// to "is a radio connected" is `client.connectedNodeNum`, and a
    /// `MeshtasticClientProtocol` is not `@Observable`, so a view
    /// reading it directly would never be invalidated when it changed —
    /// the "Connect your puck to join" banner would stay up forever
    /// after a successful connect, which is the same class of bug as
    /// the one this whole change fixes.
    private(set) var radioLink: LinkState = .disconnected
    private var linkObservation: Task<Void, Never>?

    /// Whether a radio is connected well enough for an admin write to
    /// even be attempted.
    ///
    /// `client.connectedNodeNum != nil` is not a proxy for that — it is
    /// EXACTLY the precondition both radio calls this flow makes enforce
    /// for themselves (`MeshtasticClient.requireConnectedNode()`, which
    /// gates `currentChannelTable()` and `applyChannelSet()`; the stub
    /// and demo clients check the same property). It goes non-nil only
    /// once `my_info` has landed, so it is also honest about the window
    /// where the transport is up but the handshake has not started:
    /// before that it reads `false`, which is the right answer — a write
    /// sent then would fail.
    var hasConnectedRadio: Bool {
        // Load-bearing, not decorative: reading `radioLink` is what
        // registers this computed property's SwiftUI observation
        // dependency (see that property's own doc comment). The value
        // returned is still, only, the live precondition.
        _ = radioLink
        return client.connectedNodeNum != nil
    }

    /// Starts mirroring `client.linkState()`. Idempotent, and called
    /// from `init` so no composition root has to remember to — every
    /// caller that builds a `CrewController` wants the banner to be
    /// right, and there is no case where it does not.
    private func observeRadioLink() {
        guard linkObservation == nil else { return }
        let stream = client.linkState()
        linkObservation = Task { [weak self] in
            for await state in stream {
                guard let self else { return }
                self.radioLink = state
                // A02 §3.3 amendment — the ONE thing that settles an
                // `.awaitingPuck` join: the puck came back, so read it
                // back and find out what actually took.
                if state == .ready { await self.completePendingVerification() }
            }
        }
    }

    /// The persistent banner both Start and Join show while
    /// `hasConnectedRadio` is `false` (never a toast, never only after a
    /// tap): title, one sentence of why, and a button that opens the
    /// connect step.
    static let needRadioBannerTitle = "Connect your puck to join"
    static let needRadioBannerDetail =
        "Firefly puts the crew on your puck itself, so your puck has to be connected first."
    /// What a REFUSED attempt says, as opposed to the standing banner.
    static let needRadioMessage = "Your puck isn't connected yet. Connect it, then try again."

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

    // MARK: - Meshtastic link export (§1.8 amendment, 2026-09-14)

    /// §1.8's "Copy Meshtastic link" — the connected radio's OWN,
    /// CURRENT `lora_config`, copied into the exported URL verbatim
    /// (`CrewChannel.meshtasticURL`). `nil` when that config hasn't
    /// been reported yet (`connectedNodeConfig?.loraConfig` is `nil`
    /// until at least a region has been seen) or when its region is
    /// `.unset` (`CrewChannel.ExportError.regionUnset`) — either way,
    /// the screen shows "Set the radio region first" rather than a link
    /// that would write the importing radio deaf (bench finding,
    /// `docs/specs/A02-crew-join.md` §1.8).
    func meshtasticURL(for code: CrewCode) -> String? {
        guard let lora = client.connectedNodeConfig?.loraConfig else { return nil }
        return try? CrewChannel.meshtasticURL(for: code, loraConfig: lora)
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
        observeRadioLink()
    }

    // MARK: - Shared prepare/confirm state (Start AND Join both use this)

    /// What is currently staged for the confirmation sheet — mutually
    /// exclusive with everything else in this section. `nil` once
    /// `confirmApply()`/`cancelPending()` runs.
    enum PendingKind: Equatable {
        /// `switchingFrom` is the crew this phone was on when THIS
        /// Start began, or `nil` for the ordinary case (no crew yet).
        /// Advanced -> "Start a new crew" (§6.5) is the only path that
        /// ever populates it — the confirmation sheet's extra line
        /// (`confirmationLines`) is what tells the user the old crew
        /// stops seeing them, the same treatment `.join`'s own
        /// `changingFrom` already gets for the mirror-image case.
        case start(code: CrewCode, humanName: String, switchingFrom: String?)
        case join(code: CrewCode, name: String?, changingFrom: String?)

        /// The crew code either case is staging — what `confirmApply()`
        /// verifies the radio's read-back against.
        var code: CrewCode {
            switch self {
            case .start(let code, _, _), .join(let code, _, _): return code
            }
        }
    }
    /// Where a Start/Join attempt has actually got to — the one thing
    /// both screens render their progress and failure from, so neither
    /// can ever be in the "button tapped, nothing on screen" state build
    /// 328 shipped.
    ///
    /// The three progress phases are named for steps that ACTUALLY run,
    /// not for a storyboard: `.checkingPuck` is `preparePlan()` reading
    /// the radio's live channel table, `.writing` is
    /// `applyChannelSet()` (one indivisible call that writes AND makes
    /// the radio read its own values back — the app cannot honestly
    /// narrate the inside of it), and `.verifying` is this app's own
    /// check of what came back against the code it asked for. A fourth
    /// label describing something unobserved would be exactly the kind
    /// of invented progress CLAUDE.md's honest-data rule forbids.
    enum ApplyPhase: Equatable {
        case idle
        /// An attempt was refused because no radio is connected. **No
        /// write was attempted** — `preparePlan()` is never even
        /// reached, so nothing touched the radio.
        case needsRadio
        case checkingPuck
        case writing
        case verifying
        /// A02 §3.3 amendment / A03 §3.6 amendment, 2026-09-14. The
        /// write AND the commit both reached the puck, the puck did what
        /// a commit makes it do — dropped the link and restarted — and
        /// it has not come back yet
        /// (`AdminWriteError.committedButNotVerified`).
        ///
        /// Deliberately NOT `.failed`: the bench measured this exact
        /// state being reported as "your puck didn't answer in time"
        /// while the channel was, in fact, already written and verified
        /// on the radio (2026-09-14). And deliberately NOT `.joined`
        /// either — nothing has been read back yet, and this app does
        /// not claim a crew it has not seen on the puck. It is the one
        /// honest third answer, and it is settled the moment the link
        /// returns: `completePendingVerification()` reads the puck back
        /// once and moves to `.joined` or `.failed` on what it finds.
        case awaitingPuck(String)
        case joined
        case failed(String)
    }
    private(set) var phase: ApplyPhase = .idle

    /// The one progress line the screens show, or `nil` when there is
    /// nothing in flight.
    var progressLabel: String? {
        switch phase {
        case .checkingPuck: return "Checking your puck…"
        case .writing: return "Writing to your puck…"
        case .verifying: return "Checking…"
        case .joined: return "Joined"
        // `.awaitingPuck` is not progress: nothing is running. It is a
        // standing fact with its own sentence, rendered through
        // `failureMessage` below.
        case .idle, .needsRadio, .failed, .awaitingPuck: return nil
        }
    }

    /// The honest failure text for whatever just went wrong — a radio
    /// that disconnected mid-write, a NAK, a timeout, a read-back
    /// mismatch, an UNSET region — always with something the user can
    /// do next. `nil` when nothing has failed.
    var failureMessage: String? {
        switch phase {
        case .failed(let message), .awaitingPuck(let message): return message
        case .needsRadio: return Self.needRadioMessage
        default: return nil
        }
    }

    /// Review fix (2026-09-14) — the A02 §3.3 amendment's own contract
    /// for `.awaitingPuck`, made mechanical: its Retry column is
    /// "none — Firefly settles it itself." Both the confirmation sheet's
    /// CONFIRM and `CrewApplyStatusView`'s TRY AGAIN read `failureMessage`
    /// to decide whether to show a retry control at all, and
    /// `.awaitingPuck` also has a `failureMessage` (so its sentence
    /// renders) — without this, a screen open across the puck's reboot
    /// would offer a retry that RE-SENDS the same write and reboots the
    /// puck a second time, which is the exact loop `committedButNotVerified`
    /// exists to stop. Concretely reachable: `hasConnectedRadio` goes
    /// true as soon as `my_info` lands, before the handshake reaches
    /// `.ready` — the one event that actually settles the pending join
    /// (`completePendingVerification()`) — so there is a real window
    /// where the puck reads as "connected" while this is still
    /// `.awaitingPuck`.
    var canRetry: Bool {
        if case .awaitingPuck = phase { return false }
        return true
    }

    private(set) var pending: PendingKind?
    /// A02 §3.3 amendment — the Start/Join that reached the puck but has
    /// not been read back yet (`ApplyPhase.awaitingPuck`). Held in
    /// memory rather than persisted on purpose: it is only meaningful
    /// while this app is running and connected to the puck it wrote, and
    /// a persisted one would come back after a relaunch as a claim about
    /// a radio that may since have been factory-reset, re-joined
    /// elsewhere, or handed to someone else. A relaunch starts from what
    /// the puck actually says, which is the same rule every other part
    /// of this flow follows.
    ///
    /// Cleared by exactly three things: the read-back itself (once,
    /// whatever it finds), `cancelPending()`, and a fresh
    /// `confirmApply()` — a new attempt supersedes the old one. NOT
    /// cleared by `clearFailure()`, which only resets what is on screen.
    private(set) var pendingVerification: PendingKind?
    /// Review fix (2026-09-14) — `completePendingVerification()` awaits
    /// `client.currentChannelTable()`, and this is `@MainActor`, not an
    /// actor of its own: another `@MainActor` call (a fresh
    /// `confirmApply()`, `cancelPending()`) can run to completion in
    /// that gap. Nilling `pendingVerification` up front stops a SECOND
    /// `.ready` from starting a second read, but it does nothing for a
    /// read already past that line — without this token, a stale
    /// read-back would resume after the gap and overwrite whatever the
    /// newer attempt had already decided (`phase`, `pending`, even
    /// `profile` via `adoptProfile`).
    ///
    /// Bumped by anything that supersedes an in-flight verification —
    /// `cancelPending()`, and the start of `confirmApply()` — so a
    /// verification can tell after its `await` whether it is still the
    /// one that matters and bail out (touching nothing) if not.
    private var verificationGeneration: UInt64 = 0
    private(set) var isPreparing = false
    private(set) var isApplying = false
    private(set) var errorMessage: String?
    /// Set instead of staging a join — §3.4: scanning your own code is a
    /// no-op with a friendly confirmation, never a write.
    private(set) var rejoinOwnCrewMessage: String?

    var isBusy: Bool { isPreparing || isApplying }

    /// §2.1: mint a fresh code and stage it for confirmation.
    ///
    /// §6.5's "Start a new crew" (Advanced, while already on one) calls
    /// this exact function — there is no second minting path — so
    /// `switchingFrom` is read off `profile` BEFORE `preparePlan()` can
    /// touch anything, the same "capture the previous state first"
    /// shape `beginJoin(code:name:)`'s own `changingFrom` already uses.
    @discardableResult
    func beginStart(humanName: String) async -> Bool {
        errorMessage = nil
        rejoinOwnCrewMessage = nil
        guard requireRadio() else { return false }
        let switchingFrom = profile?.humanName
        let code = CrewCode.generate()
        let trimmed = humanName.trimmingCharacters(in: .whitespacesAndNewlines)
        let name = trimmed.isEmpty ? "My crew" : trimmed
        guard await preparePlan(for: code) else { return false }
        pending = .start(code: code, humanName: name, switchingFrom: switchingFrom)
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
        // §3.4's "you're already in this crew" is answered BEFORE the
        // radio gate on purpose: it is true, and actionable, whether or
        // not a puck happens to be connected, and telling someone to go
        // connect a radio for a write that would never happen anyway
        // would be a worse answer than the real one.
        if let profile, profile.code == code.canonical {
            rejoinOwnCrewMessage = "You're already in \(profile.humanName)."
            return false
        }
        guard requireRadio() else { return false }
        let changingFrom = profile?.humanName
        guard await preparePlan(for: code) else { return false }
        pending = .join(code: code, name: name, changingFrom: changingFrom)
        return true
    }

    /// The ONE place an attempt is refused for want of a radio — and the
    /// reason it is refused HERE, before `preparePlan()`, rather than
    /// left to the radio call's own throw: a refusal that never touches
    /// the radio is the only kind this app can honestly promise
    /// attempted nothing.
    private func requireRadio() -> Bool {
        guard hasConnectedRadio else {
            phase = .needsRadio
            errorMessage = Self.needRadioMessage
            return false
        }
        return true
    }

    private func preparePlan(for code: CrewCode) async -> Bool {
        isPreparing = true
        phase = .checkingPuck
        defer { isPreparing = false }
        await snapshotCurrentPrimaryIfNeeded()
        let url = ChannelURL.encode(CrewChannel.channelSet(for: code), addMode: false)
        importer.importURL(url)
        let ok = await importer.preparePlan()
        if !ok {
            let message = importer.planErrorMessage ?? "Couldn't prepare that crew."
            errorMessage = message
            phase = .failed(message)
        }
        return ok
    }

    func cancelPending() {
        pending = nil
        pendingVerification = nil
        // Review fix (2026-09-14) — supersede any verification already
        // in flight, not only the flag a NEW one would have checked.
        verificationGeneration &+= 1
        importer.clear()
        errorMessage = nil
        phase = .idle
    }

    /// Clears a refusal/failure so a screen coming back from the connect
    /// step (or a plain TRY AGAIN) starts from a clean state rather than
    /// showing the previous attempt's words next to a fresh one.
    func clearFailure() {
        switch phase {
        // `.awaitingPuck` is not a stale error from a previous attempt,
        // it is a live fact about the puck this app is still waiting on
        // — resetting it would hide the one sentence that explains why
        // the screen is neither joined nor failed, while the read-back
        // it is waiting for is still armed underneath.
        case .joined, .awaitingPuck: return
        default: break
        }
        phase = .idle
        errorMessage = nil
        rejoinOwnCrewMessage = nil
    }

    // MARK: - Plain-language confirmation copy (§2.2, §3.3, AC8)

    /// AC8: contains none of "node", "channel", "index", "precision",
    /// "preset", "region", "PSK", "Meshtastic".
    var confirmationTitle: String {
        switch pending {
        case .start(_, let name, _): return "Start \(name)?"
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

    /// The extra plain line a CHANGE-of-crew adds (§3.4, and §6.5's
    /// "Start a new crew" — the mirror-image case) — and nothing else.
    /// An ordinary Start and a first-ever Join say everything they need
    /// to in `confirmationPrimaryText`; repeating it here would print it
    /// twice on the sheet.
    var confirmationLines: [String] {
        switch pending {
        case .start(_, let name, .some(let previous)):
            return ["This mints a brand-new crew called \(name). \(previous) stops seeing you, " +
                     "and you can only get back into \(previous) with its own code."]
        case .join(_, let name, .some(let previous)):
            let target = name ?? "the new crew"
            return ["You'll leave \(previous) and join \(target). You can come back with \(previous)'s code."]
        default:
            return []
        }
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
        // Re-checked here, not only in `beginStart`/`beginJoin`: a puck
        // can drop between the plan and the tap on CONFIRM (a pocket, a
        // flat battery, someone walking off with it), and that window is
        // exactly where a silent failure would be least explicable.
        guard requireRadio() else { return false }
        errorMessage = nil
        isApplying = true
        phase = .writing
        // A fresh attempt supersedes any read-back the previous one left
        // owing: whatever the puck ends up holding, it is this write
        // that decides it. The generation bump is what makes that true
        // even for a verification already past its `await` (review fix,
        // 2026-09-14) — see `verificationGeneration`'s own doc comment.
        pendingVerification = nil
        verificationGeneration &+= 1
        defer { isApplying = false }
        let ok = await importer.confirmApply()
        guard ok else {
            // Every honest `AdminWriteError` — a radio that disconnected
            // mid-write, a NAK/partial apply, a timeout, a read-back
            // mismatch, an UNSET region — already has its own sentence
            // in `writeMessage(for:)`. This just puts it somewhere the
            // screens can see it.
            let message = importer.applyErrorMessage ?? "Couldn't apply that crew."
            errorMessage = message
            // A02 §3.3 amendment — the one failure that is not the end
            // of the attempt. The commit reached the puck and the puck
            // restarted; the join stays PENDING a read-back rather than
            // being thrown away, and rather than being claimed.
            if importer.applyError == .committedButNotVerified {
                pendingVerification = pending
                phase = .awaitingPuck(message)
            } else {
                phase = .failed(message)
            }
            return false
        }
        // The app's own check of what the radio reported back, as
        // distinct from the radio agreeing with itself inside
        // `applyChannelSet`. Cheap (no extra round trip — this is the
        // report that call already returned) and real: a report with no
        // primary, or a primary carrying some other channel's name, is
        // not a crew this app may claim the user joined.
        phase = .verifying
        guard Self.report(importer.lastAppliedReport, carries: pending.code) else {
            let message = "Your puck didn't come back with \(pending.code.canonical). " +
                "Nothing is certain until it does — try again."
            errorMessage = message
            phase = .failed(message)
            return false
        }
        switch pending {
        case .start(let code, let name, _):
            adoptProfile(code: code, humanName: name)
        case .join(let code, let name, _):
            adoptProfile(code: code, humanName: name ?? code.canonical)
        }
        self.pending = nil
        phase = .joined
        return true
    }

    /// Pure, so it is testable without a client: the written primary's
    /// name IS the crew code (§1.3, "why the channel name is the code"),
    /// so a read-back that carries it is the whole verification.
    static func report(_ report: ChannelWriteReport?, carries code: CrewCode) -> Bool {
        guard let report else { return false }
        guard let primary = report.channels.first(where: { $0.role == .primary }) else { return false }
        return primary.settings.name == code.canonical
    }

    /// A02 §3.3 amendment — the read-back that settles an
    /// `.awaitingPuck` join, run ONCE, on the first `.ready` after the
    /// puck's post-commit restart.
    ///
    /// This is the whole reason `committedButNotVerified` is allowed to
    /// leave a join pending instead of failing it: the app never claims
    /// the crew took, it goes and looks. What it looks for is the crew
    /// channel by **name and key** (`channelCarries(_:code:)`) — the
    /// name alone is public, printed on a screen and read out loud
    /// across tents, so a puck sitting on a channel that merely shares
    /// the name is not this crew.
    ///
    /// `pendingVerification` is consumed before the read, not after:
    /// "once" has to survive the `await` in the middle, and a second
    /// `.ready` arriving during the read must not start a second one.
    ///
    /// Review fix (2026-09-14) — that alone stops a SECOND `.ready` from
    /// starting a second read, but it does nothing about a read already
    /// past the `await` below: this is `@MainActor`, not an actor of its
    /// own, so a fresh `confirmApply()` or a `cancelPending()` can run
    /// to completion in that gap. `verificationGeneration` is captured
    /// before the read and re-checked after it — anything that
    /// supersedes this attempt bumps it, and a stale read that finds it
    /// changed bails out without touching `phase`/`pending`/`profile`.
    private func completePendingVerification() async {
        guard let staged = pendingVerification else { return }
        pendingVerification = nil
        let generation = verificationGeneration
        let code = staged.code
        guard let table = try? await client.currentChannelTable() else {
            guard generation == verificationGeneration else { return }
            // The puck is back but would not tell us what it is holding.
            // Nothing is known, so nothing is claimed.
            let message = "Your puck came back, but Firefly couldn't read the crew off it — " +
                "try again."
            errorMessage = message
            phase = .failed(message)
            return
        }
        guard generation == verificationGeneration else { return }
        guard Self.table(table, carries: code) else {
            let message = "Your puck didn't come back with \(code.canonical). " +
                "Nothing is certain until it does — try again."
            errorMessage = message
            phase = .failed(message)
            return
        }
        switch staged {
        case .start(let code, let name, _):
            adoptProfile(code: code, humanName: name)
        case .join(let code, let name, _):
            adoptProfile(code: code, humanName: name ?? code.canonical)
        }
        pending = nil
        errorMessage = nil
        phase = .joined
    }

    /// Pure, so it is testable without a client: does this channel table
    /// actually carry the crew — primary slot, the code as the name, and
    /// the key that code derives (§1.4/§1.5)?
    ///
    /// Stricter than `report(_:carries:)` above on purpose. That one
    /// checks a report this app just got back from a write it just sent,
    /// inside one `applyChannelSet` call. This one checks a puck that
    /// has been away, rebooted, and come back — possibly a puck somebody
    /// else re-provisioned in the meantime — so the key has to match
    /// too, not just the label.
    static func table(_ channels: [Channel], carries code: CrewCode) -> Bool {
        guard let primary = channels.first(where: { $0.role == .primary }) else { return false }
        let expected = CrewChannel.channelSettings(for: code)
        return primary.settings.name == expected.name && primary.settings.psk == expected.psk
    }

    private func adoptProfile(code: CrewCode, humanName: String) {
        if let old = profile {
            profileStore.rememberRecentCrew(RecentCrew(code: old.code, humanName: old.humanName))
        }
        let new = CrewProfile(code: code.canonical, humanName: humanName, createdAtMs: clock())
        profileStore.save(new)
        profile = new
        onProfileChanged?()
    }

    // MARK: - Leave (§3.4)

    private(set) var leaveErrorMessage: String?

    /// §3.4: writes the radio back — the pre-crew snapshot if this phone
    /// has one, else the stock default primary (`position_precision ==
    /// 0`). Never a local-only forget.
    @discardableResult
    func leaveCrew() async -> Bool {
        guard let profile else { return false }
        // Leave is a WRITE (§3.4's "leaving writes the radio back, it is
        // not a local forget"), so it needs the same up-front refusal
        // Start/Join get — a local-only forget while the puck kept
        // transmitting on the crew channel is the outcome §3.4 calls
        // the worst possible one.
        guard hasConnectedRadio else {
            leaveErrorMessage = "Your puck isn't connected. Leaving has to be written to your " +
                "puck, so connect it first."
            return false
        }
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
        onProfileChanged?()
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
