//
//  CrewMembershipEngine.swift — A02 slice C: "a node becomes crew when
//  this radio delivers us a packet it decrypted with our crew channel's
//  key, from an id that is not us and not hidden, on one of four
//  portnums" (docs/specs/A02-crew-join.md §4.1).
//
//  This is the app half of the same policy change S02's 2026-09-13
//  amendment makes on the puck, and it is a policy change, not a flag:
//  `ff_shell.h`'s ROSTER TRUST POLICY said the paired roster never grows
//  from anything the radio says, because membership had no other
//  definition. A02 gives it one — possession of the crew channel's key
//  — so the amended sentence reads: *the roster grows from an explicit
//  user action, or from proof of the crew key, and from nothing else.*
//
//  WHAT THIS TYPE DELIBERATELY DOES NOT DO
//
//   * It never writes a name into `ff_crew`. §4.4's "New crew member"
//     is a DISPLAY fallback (`fallbackDisplayName` below), resolved at
//     render time in the order nickname -> long name -> short name ->
//     fallback. Writing it into `ff_crew_member_t.long_name` would put
//     a fabricated name in the model, where a later real NodeInfo could
//     not tell it apart from a name the mesh actually reported — the
//     exact failure `CrewPairingRecord.nickname`'s own doc comment
//     already refuses for nicknames.
//   * It never fabricates a position, a timestamp or a presence. An
//     admitted member with no traffic yet reads "waiting to hear from
//     them" because `ff_crew` genuinely holds nothing for them.
//   * It never falls back to channel index 0. See `channelStatus`.
//
import FireflyCore
import FireflyMesh
import Foundation
import MeshtasticProto

@MainActor
public final class CrewMembershipEngine: CrewMembershipGating, CrewMembershipProviding,
    CrewHeardListProviding, CrewDiagnosticsProviding {
    // MARK: - The rule's constants

    /// A02 §4.1 clause 6 — the four portnums that admit, BY RAW VALUE.
    ///
    /// 269 is Firefly's own private portnum (`firmware/core/include/
    /// ff_proto.h`). It is **not** `PRIVATE_APP`: in `portnums.proto`
    /// `PRIVATE_APP` is 256 and `ATAK_FORWARDER` is 257, and 269 is not
    /// a named enumerator at all — it arrives as
    /// `PortNum.UNRECOGNIZED(269)`. Matching the `.privateApp` case
    /// instead would silently admit nobody, which is why this set is
    /// raw values and `MeshRxMeta.portnum` is a raw value too.
    ///
    /// `TELEMETRY_APP` is deliberately absent: telemetry refreshes an
    /// existing member's presence (the unconditional `ff_crew_on_heard`
    /// on the puck) but carries neither identity nor intent, and
    /// NodeInfo follows within minutes anyway (§4.2).
    public static let admittingPortnums: Set<Int32> = [
        Int32(PortNum.textMessageApp.rawValue),   // 1
        Int32(PortNum.positionApp.rawValue),      // 3
        Int32(PortNum.nodeinfoApp.rawValue),      // 4
        Int32(fireflyPrivatePortNum),             // 269
    ]

    /// §4.4's last resort. Never blank, never a hex id, and never
    /// written into the model — see this file's header comment.
    public static let fallbackDisplayName = "New crew member"

    /// The same bound `NearbyNodesViewModel.maxUnpairedTracked` uses,
    /// for the same reason and reusing its reasoning: an app-side list
    /// of ids heard on a busy mesh has to be bounded, and 64 is sized
    /// for a phone's scrollable list rather than the puck's DRAM.
    /// §4.3 asks for exactly this ("app-side, bounded, and LRU-evicted
    /// exactly like `NearbyNodesViewModel`'s own `maxUnpairedTracked`").
    public static let maxUntrackedTracked = 64

    // MARK: - Collaborators

    /// The ONE writer of `ff_crew`'s paired flag (`CrewPairingStore
    /// .swift`) — auto-admission routes through it unchanged, so there
    /// is still exactly one place the roster grows, and its existing
    /// `.full(limit:)` result is what drives §4.3's overflow banner.
    private let pairing: CrewPairingController
    private let store: any CrewLocalStateStoring
    private let client: any MeshtasticClientProtocol
    /// Injectable for tests; `Date.init` in every real composition.
    private let now: @Sendable () -> Date

    // MARK: - State

    public private(set) var crewChannel: CrewChannelIdentity?
    public private(set) var channelStatus: CrewChannelStatus = .noCrew
    public private(set) var untracked: [UntrackedCrewMember] = []
    public private(set) var joinedSinceCreated: [CrewJoinEvent] = []
    public private(set) var hidden: [UInt32] = []
    /// A02 §6.5's "Crew diagnostics" row (`CrewDiagnosticsProviding`) —
    /// admitted/refused-by-reason since this engine was created. Never
    /// persisted, never consulted by `admits(_:)` itself: pure
    /// read-only bookkeeping alongside the real decision, not a second
    /// vote in it.
    public private(set) var admissionCounters = CrewAdmissionCounters()
    /// Epoch ms of the most recent admission, or `nil` if none has
    /// happened yet this session — never 0 (`CrewHeardListProviding
    /// .swift`'s own header comment on why UNKNOWN is never rendered as
    /// a fabricated zero).
    public private(set) var lastAdmissionAtMs: UInt64?
    private var hiddenSet: Set<UInt32> = []
    private var linkObservation: Task<Void, Never>?
    private var resolveTask: Task<Void, Never>?
    /// Bench finding 2026-09-14 (§4.4) — see `CrewNodeInfoRequestThrottle`'s
    /// own header comment. Scoped to one crew, same as
    /// `admissionCounters`/`lastAdmissionAtMs`: reset in `configure(crew:)`
    /// so a stale entry from a PREVIOUS crew's member id can never
    /// suppress a legitimate first ask on a new one.
    private var nodeInfoRequestThrottle = CrewNodeInfoRequestThrottle()

    public init(pairing: CrewPairingController,
                store: any CrewLocalStateStoring,
                client: any MeshtasticClientProtocol,
                now: @escaping @Sendable () -> Date = Date.init) {
        self.pairing = pairing
        self.store = store
        self.client = client
        self.now = now
        loadLocalState()
    }

    deinit {
        linkObservation?.cancel()
        resolveTask?.cancel()
    }

    /// The namespace the hide list and join log are stored under — the
    /// crew code, or `CrewLocalState.noCrewCode` before there is one.
    private var stateNamespace: String { crewChannel?.code ?? CrewLocalState.noCrewCode }

    private func loadLocalState() {
        hidden = store.hiddenIDs(crewCode: stateNamespace)
        hiddenSet = Set(hidden)
        joinedSinceCreated = store.joinEvents(crewCode: stateNamespace)
    }

    // MARK: - Configuration

    /// Point this engine at a crew (or at none). Called by slice B's
    /// Start/Join/Leave flows, and at launch from whatever persisted the
    /// current crew.
    ///
    /// Changing crews reloads the hide list and join log for the NEW
    /// code — §4.5's "stored per crew code, so leaving and rejoining a
    /// crew restores the hides you had" — and re-resolves the channel
    /// index from scratch. It does NOT unpair anybody: §4.6 grandfathers
    /// every pre-existing member rather than silently deleting the
    /// user's crew.
    public func configure(crew: CrewChannelIdentity?) {
        guard crew != crewChannel else { return }
        crewChannel = crew
        loadLocalState()
        // PR #313 review. The counters and `lastAdmissionAtMs` are
        // scoped to ONE crew, and this is the moment that scope ends:
        // carrying them across a Leave or a "Start a new crew" would put
        // the OLD crew's refusals and — worse — its "last admission
        // 2 min ago" on a brand-new crew nobody has joined yet, which is
        // a fabricated freshness claim of exactly the kind §4.4/§6.5
        // refuse everywhere else. Zero here is honest: this crew really
        // has admitted nobody, and "Never" is what the row reads.
        //
        // Only on a genuine CHANGE (the `guard` above), so a redundant
        // `configure` with the same identity — which `syncCrewMembership
        // WithProfile` can legitimately make, e.g. on a rename — never
        // silently resets a live session's counts.
        admissionCounters = CrewAdmissionCounters()
        lastAdmissionAtMs = nil
        nodeInfoRequestThrottle = CrewNodeInfoRequestThrottle()
        channelStatus = crew == nil ? .noCrew : .resolving
        resolveCrewChannelIndex()
    }

    /// Subscribe to the link so the index is re-resolved on every
    /// reconnect (AC14). Idempotent, the convention every `observe()` in
    /// this app follows.
    public func observe() {
        guard linkObservation == nil else { return }
        let states = client.linkState()
        linkObservation = Task { [weak self] in
            for await state in states {
                guard let self else { return }
                guard state == .ready else { continue }
                // A reconnect may be to a DIFFERENT radio, or to the
                // same radio after a CLI/stock-app reprovision. The
                // cached index is an assumption about one link and dies
                // with it.
                self.channelStatus = self.crewChannel == nil ? .noCrew : .resolving
                self.resolveCrewChannelIndex()
            }
        }
    }

    public func stopObserving() {
        linkObservation?.cancel(); linkObservation = nil
        resolveTask?.cancel(); resolveTask = nil
    }

    /// Re-read the radio's channel table and find the crew's index by
    /// name AND PSK (§4.2, AC14). Also the hook slice B calls after any
    /// admin write, since a write can move the channel.
    public func resolveCrewChannelIndex() {
        resolveTask?.cancel()
        guard crewChannel != nil else {
            channelStatus = .noCrew
            return
        }
        resolveTask = Task { [weak self] in
            await self?.refreshCrewChannelIndex()
        }
    }

    /// The awaitable form. Slice B's write path wants this (a write can
    /// move the channel, and the confirmation should not race the
    /// re-read), and so does any caller that needs the answer before
    /// continuing.
    public func refreshCrewChannelIndex() async {
        guard crewChannel != nil else {
            channelStatus = .noCrew
            return
        }
        let table = (try? await client.currentChannelTable()) ?? []
        guard !Task.isCancelled else { return }
        applyChannelTable(table)
    }

    /// Resolve the crew's index against a channel table the caller
    /// already holds — `applyChannelSet`'s own `ChannelWriteReport`
    /// carries one, so slice B's write path can re-resolve with no
    /// second round trip, and a test can drive this with no radio at
    /// all.
    ///
    /// A table that simply has not arrived yet (`[]`) leaves the status
    /// `.resolving` rather than claiming the puck is off the crew
    /// channel — "not read yet" and "read, and it isn't there" are two
    /// different honest answers, and only the second one deserves the
    /// Crew page's "Your puck isn't on this crew's channel".
    public func applyChannelTable(_ table: [Channel]) {
        guard let crew = crewChannel else {
            channelStatus = .noCrew
            return
        }
        guard !table.isEmpty else {
            channelStatus = .resolving
            return
        }
        let match = table.first {
            $0.hasSettings && $0.settings.name == crew.code && $0.settings.psk == crew.psk
        }
        if let match {
            channelStatus = .resolved(index: UInt32(bitPattern: match.index))
        } else {
            channelStatus = .notOnCrewChannel
        }
    }

    // MARK: - The gate (AC13) and the rule (AC11/AC12)

    public func admits(_ snapshot: MeshNodeSnapshot) -> Bool {
        let id = snapshot.num
        // `from == 0` is "sender unknown" on the wire, never a node.
        guard id != 0 else { return false }
        // Hidden beats everything, including "already crew": §4.1 clause
        // 4 is what stops a hidden node being silently re-admitted by
        // its very next packet, and `hide` already unpaired them.
        guard !hiddenSet.contains(id) else {
            admissionCounters.refusedHidden += 1
            return false
        }
        // Already crew — including every pre-existing, manually paired
        // member (§4.6: nothing is removed on upgrade). This is the
        // clause that makes the gate a MEMBERSHIP gate rather than an
        // admission gate: a member's telemetry, position and NodeInfo
        // all keep flowing, on any channel, exactly as before A02.
        if isCrew(id) { return true }
        return tryAdmit(snapshot)
    }

    /// Read off the LIVE roster (`ff_crew`), not off `UserDefaults`:
    /// `CrewPairingRestorer` replays the persisted paired list onto
    /// `ff_crew` at launch before anything can observe a client, so the
    /// two agree — and this way the gate costs an O(FF_CREW_MAX) C
    /// lookup per packet instead of a JSON decode.
    private func isCrew(_ nodeID: UInt32) -> Bool {
        pairing.crew.member(nodeID: nodeID, now: FireflyClock.nowMillis())?.paired == true
    }

    /// A02 §4.1, clause by clause. Every `return false` here is its own
    /// acceptance criterion in AC12.
    private func tryAdmit(_ snapshot: MeshNodeSnapshot) -> Bool {
        // Clause 1 — it reached us DECRYPTED. A `nil` rxMeta is the
        // want_config nodeDB REPLAY: a synthesized summary that cannot
        // prove the node was ever heard on our channel (§4.2). No
        // separate guard is needed for the replay, and that is the
        // point: it is structurally unable to admit anyone.
        guard let meta = snapshot.rxMeta, meta.decrypted else {
            admissionCounters.refusedNotDecrypted += 1
            return false
        }
        // Clause 2 — the index our crew channel occupies on THIS radio,
        // read from the live channel table. `.resolving`/`.noCrew`/
        // `.notOnCrewChannel` all admit nobody; there is no fallback to
        // index 0 anywhere in this file (AC14).
        guard case .resolved(let crewIndex) = channelStatus else {
            admissionCounters.refusedChannelNotResolved += 1
            return false
        }
        guard meta.channelIndex == crewIndex else {
            admissionCounters.refusedWrongChannel += 1
            return false
        }
        // Clause 3 — not us. `connectedNodeNum` is nil before the
        // handshake names one, and an unknown self id is a reason to
        // admit nobody rather than to guess.
        guard let me = client.connectedNodeNum else {
            admissionCounters.refusedSelfOrUnknownRadio += 1
            return false
        }
        guard meta.from != me, snapshot.num != me else {
            admissionCounters.refusedSelfOrUnknownRadio += 1
            return false
        }
        // Clause 5 — never over MQTT. They may well hold our PSK if
        // someone bridged the crew, but a crew is people who are HERE,
        // and an MQTT path can replay.
        guard !meta.viaMQTT else {
            admissionCounters.refusedViaMQTT += 1
            return false
        }
        // Clause 6 — one of the four portnums, by raw value.
        guard let portnum = meta.portnum, Self.admittingPortnums.contains(portnum) else {
            admissionCounters.refusedWrongPortnum += 1
            return false
        }

        return admit(nodeID: snapshot.num)
    }

    /// Pairs the node through the one audited growth path, or records it
    /// honestly as untracked when the roster is full (§4.3). Returns
    /// whether the node may now be fed into `ff_crew`.
    private func admit(nodeID: UInt32) -> Bool {
        switch pairing.pair(nodeID: nodeID) {
        case .paired:
            untracked.removeAll { $0.nodeID == nodeID }
            recordJoin(nodeID: nodeID)
            admissionCounters.admitted += 1
            lastAdmissionAtMs = UInt64((now().timeIntervalSince1970 * 1000).rounded())
            requestNodeInfoIfNameless(nodeID: nodeID)
            return true
        case .full:
            // NOT a silent drop — that is the behaviour §4.3 forbids.
            noteUntracked(nodeID: nodeID)
            admissionCounters.refusedRosterFull += 1
            return false
        }
    }

    /// Bench finding 2026-09-14 (§4.4, mirroring firmware's
    /// `shell_try_admit`): right after a successful admission, ask the
    /// member directly for its NodeInfo rather than waiting for its own
    /// periodic broadcast (Meshtastic's stock interval is on the order
    /// of hours).
    ///
    /// "Nameless" is read fresh off the ROSTER (`pairing.crew.member`),
    /// never off the packet that triggered this admission — same as
    /// firmware's own comment on `shell_try_admit`: an UNHIDDEN member's
    /// slot can already carry a name from before it was hidden (hide =
    /// unpair, never erase — `CrewPairingController.unpair`'s own doc
    /// comment: it removes the persisted record but never touches
    /// `ff_crew`'s name fields, and `pair(nodeID:)` re-finds the SAME
    /// `ff_crew` slot by node id rather than recreating one), in which
    /// case this must NOT ask again. That fact lives on the roster, not
    /// on whatever packet happened to re-admit them — a Position or Text
    /// re-admission carries no name to check in the first place.
    ///
    /// Corollary, carried over from firmware unchanged and worth stating
    /// rather than quietly "fixing": `CoreStore.apply(nodeUpdate:)`
    /// writes `crew.setIdentity` from the admitting snapshot only AFTER
    /// `admits(_:)` returns, so a FRESH admission via a live NodeInfo
    /// packet itself still reads as nameless here and does ask — one
    /// harmless, rate-limited request whose answer the admitting packet
    /// already made redundant. `shell_try_admit` has the identical
    /// ordering (rx-meta admits before the portnum-specific payload
    /// names) and makes the identical call; this keeps the two
    /// implementations in the same place rather than inventing a
    /// snapshot-inspecting shortcut firmware does not have.
    private func requestNodeInfoIfNameless(nodeID: UInt32) {
        let member = pairing.crew.member(nodeID: nodeID, now: FireflyClock.nowMillis())
        guard member?.displayName.isEmpty ?? true else { return }
        guard nodeInfoRequestThrottle.shouldSend(nodeID: nodeID, now: now()) else { return }
        // Fire-and-forget, same discipline `ThreadViewModel`'s FLARE/
        // FIND pings already follow for an ask nobody is blocked on: the
        // reply (or its absence) arrives on `nodeUpdates()` through the
        // ordinary live-NodeInfo decode path, not through this call's
        // return value. A failed send is swallowed deliberately — see
        // `CrewNodeInfoRequestThrottle.shouldSend(nodeID:now:)`'s own
        // doc comment on why the throttle already recorded the attempt
        // regardless of whether the radio call below succeeds.
        Task { [weak self] in
            _ = try? await self?.client.requestNodeInfo(from: nodeID)
        }
    }

    private func recordJoin(nodeID: UInt32) {
        guard !joinedSinceCreated.contains(where: { $0.nodeID == nodeID }) else { return }
        joinedSinceCreated.append(CrewJoinEvent(nodeID: nodeID, joinedAt: now()))
        store.setJoinEvents(joinedSinceCreated, crewCode: stateNamespace)
    }

    /// Bounded LRU, oldest-heard evicted first — the same policy and the
    /// same bound `NearbyNodesViewModel.enforceUnpairedBound` applies.
    private func noteUntracked(nodeID: UInt32) {
        let stamp = now()
        if let idx = untracked.firstIndex(where: { $0.nodeID == nodeID }) {
            let existing = untracked[idx]
            untracked[idx] = UntrackedCrewMember(nodeID: nodeID, firstHeard: existing.firstHeard,
                                                  lastHeard: stamp)
        } else {
            untracked.append(UntrackedCrewMember(nodeID: nodeID, firstHeard: stamp, lastHeard: stamp))
        }
        guard untracked.count > Self.maxUntrackedTracked else { return }
        // A TOTAL order, for the same reason `currentMembers()` needs
        // one: `sort(by:)` is not stable in Swift, so two ids last heard
        // in the same instant would otherwise decide between themselves
        // differently run to run — and this list is rendered ("Not
        // tracked (N)", §4.3), so an arbitrary order is a UI that
        // reshuffles for no reason. Ties fall back to the node id.
        untracked.sort { $0.lastHeard == $1.lastHeard ? $0.nodeID < $1.nodeID : $0.lastHeard < $1.lastHeard }
        untracked.removeFirst(untracked.count - Self.maxUntrackedTracked)
    }

    // MARK: - Membership readout

    /// `CrewMembershipProviding` (slice B's seam) — "admitted since the
    /// crew was created, newest first" (§2.3), which is what the Start
    /// screen's *Joined · N* list and the Crew page's People list both
    /// render.
    ///
    /// Honest about every unknown, per that type's own doc comments:
    /// `displayName` is `nil` (never a fabricated string, and never a
    /// hex id) until some name has actually arrived — slice B renders
    /// §4.4's "New crew member" for it; `joinedAtMs` is `nil` for a
    /// member this phone never observed joining, which is every
    /// grandfathered §4.6 member and every member restored from
    /// persistence before a packet.
    ///
    /// Hidden members are absent, because hiding unpairs them (§4.5) —
    /// there is no separate filter to forget to apply.
    public func currentMembers() -> [CrewJoinedMember] {
        let nowMs = FireflyClock.nowMillis()
        let joined = Dictionary(joinedSinceCreated.map { ($0.nodeID, $0.joinedAt) },
                                 uniquingKeysWith: { first, _ in first })
        let rows = pairing.pairedRecords().map { record -> CrewJoinedMember in
            let member = pairing.crew.member(nodeID: record.nodeID, now: nowMs)
            let meshName = (member?.displayName.isEmpty ?? true) ? nil : member?.displayName
            let joinedAt = joined[record.nodeID]
            return CrewJoinedMember(
                id: record.nodeID,
                displayName: record.nickname ?? meshName,
                colorIndex: record.colorIndex,
                joinedAtMs: joinedAt.map { UInt64(($0.timeIntervalSince1970 * 1000).rounded()) },
                heardPresence: member?.heardPresence ?? .never,
                // PR #308 review. The presence WORDS are age-carrying by
                // rule ("6 min ago", "No signal \u{00B7} 40 min" —
                // `PresenceTag.plainLabel(age:)`), so a row without an
                // age falls back to the bare enum name. This is
                // `ff_crew`'s own `last_heard_ms`, nil when the member
                // has never been heard — never 0, which would render as
                // "just now".
                heardAgeMs: member?.heardAgeMs)
        }
        // Newest join first; members with no observed join time (§4.6's
        // "From before", and anything restored before a packet) sort
        // last rather than being given an invented timestamp to sort by.
        // A TOTAL order, deliberately: `sorted(by:)` is not stable in
        // Swift, so two members admitted inside the same millisecond
        // would otherwise come back in an arbitrary order that differs
        // run to run (caught by this file's own test under a thread
        // sanitizer). Ties fall back to the node id.
        return rows.sorted {
            switch ($0.joinedAtMs, $1.joinedAtMs) {
            case let (l?, r?): return l == r ? $0.id < $1.id : l > r
            case (nil, _?): return false
            case (_?, nil): return true
            case (nil, nil): return $0.id < $1.id
            }
        }
    }

    public var members: [CrewMembershipRecord] {
        let joined = Dictionary(joinedSinceCreated.map { ($0.nodeID, $0.joinedAt) },
                                 uniquingKeysWith: { first, _ in first })
        return pairing.pairedRecords().map { record in
            CrewMembershipRecord(nodeID: record.nodeID,
                                  colorIndex: record.colorIndex,
                                  nickname: record.nickname,
                                  origin: joined[record.nodeID].map(CrewMemberOrigin.joinedCrew) ?? .fromBefore)
        }
    }

    /// §4.4's display-name order, in one place so no screen invents its
    /// own. `longName`/`shortName` are whatever the MESH reported; the
    /// fallback is reached only when nothing has.
    public static func displayName(nickname: String?, longName: String?, shortName: String?) -> String {
        for candidate in [nickname, longName, shortName] {
            if let candidate, !candidate.isEmpty { return candidate }
        }
        return fallbackDisplayName
    }

    // MARK: - Hide (§4.5)

    public func isHidden(_ nodeID: UInt32) -> Bool { hiddenSet.contains(nodeID) }

    /// Hide = **unpair + remember**. Deliberately the same mechanism the
    /// cap uses (§4.3), because freeing a roster slot is the whole
    /// reason hiding is useful to a crew of nine. Nothing is
    /// transmitted: on a mesh where possession of the key is membership
    /// there is no "kick", and a UI implying otherwise would be lying
    /// about what the radio is doing.
    public func hide(nodeID: UInt32) {
        pairing.unpair(nodeID: nodeID)
        untracked.removeAll { $0.nodeID == nodeID }
        guard !hiddenSet.contains(nodeID) else { return }
        hidden.append(nodeID)
        hiddenSet.insert(nodeID)
        store.setHiddenIDs(hidden, crewCode: stateNamespace)
    }

    public func unhide(nodeID: UInt32) {
        guard hiddenSet.contains(nodeID) else { return }
        hidden.removeAll { $0 == nodeID }
        hiddenSet.remove(nodeID)
        store.setHiddenIDs(hidden, crewCode: stateNamespace)
        // Nothing else: they come back on their next qualifying packet.
        // Re-pairing them here would claim a presence nobody has
        // observed since the hide.
    }

    // MARK: - Demo-only seeding (§4.3/§4.7, `-FireflyDemoScreen crew`)

    /// Appends a fabricated overflow entry directly, bypassing
    /// `tryAdmit`/`admit` entirely — the same way `DemoRunner` seeds
    /// CAMP's position straight onto `ff_crew` instead of synthesizing a
    /// radio packet for a landmark that has no radio (`DemoRunner
    /// .swift`'s own comment on that seam). A real "Not tracked" entry
    /// requires `FF_CREW_MAX` (8) real, qualifying packets to arrive
    /// first; a demo world that actually did that just to populate one
    /// screenshot row would need eight fictional crew members with
    /// nothing else to justify their existing. This can never be
    /// reached from a real packet — nothing in `admits(_:)`/`tryAdmit`
    /// calls it — and it never touches `admissionCounters`, which stays
    /// an honest count of real admission decisions even in demo mode.
    public func seedUntrackedForDemo(nodeID: UInt32, firstHeard: Date, lastHeard: Date) {
        untracked.removeAll { $0.nodeID == nodeID }
        untracked.append(UntrackedCrewMember(nodeID: nodeID, firstHeard: firstHeard, lastHeard: lastHeard))
    }
}
