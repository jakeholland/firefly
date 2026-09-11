//
//  DemoRunner.swift — the S20 honesty rule's app-side driver: plays
//  Firefly Fields' scripted timeline through the SAME `AppGraph`/
//  `CoreStore`/`ff_crew`+`ff_feed` bridges the live graph uses. This
//  type is the only thing in `FireflyModel` allowed to know the demo
//  world exists; nothing it touches (`CoreStore`, the view models) has
//  a "demo" branch anywhere in it — they cannot tell a
//  `DemoMeshtasticClient` from a real one, which is exactly the point
//  (`DemoMeshtasticClient.swift`'s own header comment).
//
//  Why crew PAIRING is seeded straight onto `core.crew` rather than
//  through `nodeUpdates()`: M1's own "Add to crew" UI is still
//  session-local and does not call `ff_crew_set_paired`
//  (`NearbyNodesViewModel.swift`'s "LOCAL AND SESSION-ONLY for M1" —
//  the real wiring is a later milestone). A demo that only drove
//  `nodeUpdates()` would therefore never show a paired member on Radar
//  at all — an empty ring, same as the stub. Calling
//  `core.crew.setPaired` directly is not a shortcut around the real
//  API: it is the SAME `ff_crew_set_paired` that UI will call once it
//  exists, exercised here because M1 has not built the tap that reaches
//  it yet.
//
//  Why Mo gets NO `nodeUpdates()` entry at all: `has_heard` is a sticky
//  "has ANY packet, ever, arrived from this node" flag
//  (`firmware/core/include/ff_crew.h`'s HEARD-presence doc comment) —
//  once true it never goes false again in this session. `RADAR_LOST`
//  only fires for a paired member with `has_heard == false`
//  (`firmware/core/src/ff_radar.c`), so the one honest way to show it
//  is a member `ff_crew_set_paired` has created a slot for and that has
//  never otherwise been mentioned — not a fake "hasn't been heard in a
//  while" position, an actual never-once-heard radio silence.
//
import FireflyMesh
import Foundation

@MainActor
public final class DemoRunner {
    private let graph: AppGraph
    private let client: DemoMeshtasticClient
    private let location: DemoLocationProvider
    private let heading: DemoHeadingProvider
    private let connect: ConnectViewModel
    private let inbox: InboxViewModel
    private let radar: RadarViewModel
    private let world: DemoWorld
    /// Taylor's thread, opened and `observe()`d as part of `start()` —
    /// BEFORE `client.connect()` — rather than at first send time. Its
    /// `isLinkReady` only ever flips true off a `.ready` `linkState()`
    /// event it personally subscribed to (`ThreadViewModel.observe()`'s
    /// own `EventHub` subscription); `EventHub` never replays a past
    /// event to a late subscriber (`EventHub.swift`'s own doc comment),
    /// so a `ThreadViewModel` opened AFTER `connect()` had already
    /// yielded `.ready` would sit on `isLinkReady == false` forever and
    /// queue both scripted DMs instead of sending either.
    private var taylorThread: ThreadViewModel?
    /// Flips true at the very end of `start()`. `RootView`'s own
    /// `-FireflyDemoScreen` handling (`withdrawPhoneFix()`/
    /// `startFindOnTaylor()`) runs from a SEPARATE `.task` than
    /// `FireflyApp`'s `await graph.start(); await demoRunner?.start()`
    /// chain — two independent tasks with no ordering guarantee between
    /// them otherwise — so `waitUntilStarted()` is what stops
    /// `RootView` from calling `withdrawPhoneFix()` before `start()`'s
    /// own `location.setFix(world.phoneFix)` has even run (which would
    /// have the LATER call silently put the fix right back).
    public private(set) var isStarted = false

    public init(graph: AppGraph, client: DemoMeshtasticClient, location: DemoLocationProvider,
                heading: DemoHeadingProvider, connect: ConnectViewModel, inbox: InboxViewModel,
                radar: RadarViewModel, world: DemoWorld = .fireflyFields()) {
        self.graph = graph
        self.client = client
        self.location = location
        self.heading = heading
        self.connect = connect
        self.inbox = inbox
        self.radar = radar
        self.world = world
    }

    /// Runs once per launch. Subscribes every screen's own view model
    /// BEFORE anything is yielded (`EventHub`'s "a subscriber that
    /// arrives after this call does not see it" rule —
    /// `EventHub.swift`), connects the scripted client, seeds crew
    /// pairing and the CAMP landmark, then plays the timeline: an
    /// incoming text from Taylor, a second RSSI sample so the signal
    /// view has a trend, and two outgoing DMs — one that resolves
    /// DELIVERED, one that resolves NO ACK.
    public func start() async {
        connect.observe()
        inbox.observe()
        radar.observe()
        let thread = inbox.openThread(.member(DemoCrew.taylor))
        thread.observe()
        taylorThread = thread
        client.onSendPrivate = { [weak self] payload, destination, wantAck in
            // `onSendPrivate` fires on whatever isolation domain called
            // `sendText`/`sendPrivate` — never assumed to be this
            // `@MainActor` type's own. Hop over explicitly rather than
            // making the hook itself `@MainActor` (it has to stay
            // callable from `DemoMeshtasticClient`, which is not).
            Task { @MainActor in
                self?.replyToFindPingIfNeeded(payload: payload, destination: destination, wantAck: wantAck)
            }
        }

        try? await client.connect()

        // Taylor, Dana, Sam: real crew, paired the moment they are
        // known (mirrors what a "you two are now crew" tap will do once
        // M2 wires it — see this file's header comment).
        graph.core.crew.setPaired(nodeID: DemoCrew.taylor, paired: true)
        graph.core.crew.setPaired(nodeID: DemoCrew.dana, paired: true)
        graph.core.crew.setPaired(nodeID: DemoCrew.sam, paired: true)

        // Mo: paired, never heard — the one honest RADAR_LOST case
        // (this file's header comment). No nodeDB entry, ever.
        graph.core.crew.setPaired(nodeID: DemoCrew.mo, paired: true)

        // CAMP: an asserted landmark, seeded the same way a real
        // "somebody typed this in" position would be
        // (`NodePosition.Source.manual`'s own doc comment) — onto
        // `ff_crew` directly, never through a radio packet, because a
        // landmark has no radio to send one. Deliberately NOT the venue
        // anchor itself (the phone's own demo fix): `ff_crew_close_range`
        // (`ff_crew.h`) treats anything under 30 m as RADAR_CLOSE before
        // PLACE is ever considered, and a landmark sitting exactly on top
        // of "my" position would make that the honest answer too — just
        // not the one this screenshot needs.
        graph.core.crew.setIdentity(nodeID: DemoCrew.camp, shortName: "CAMP", longName: "Camp Firefly")
        graph.core.crew.onPosition(
            nodeID: DemoCrew.camp, latitude: DemoWorld.campLatitude, longitude: DemoWorld.campLongitude,
            rxTimeMs: FireflyClock.nowMillis(),
            meta: CrewStore.PositionMeta(asserted: true, precisionBits: nil))
        graph.core.crew.setPaired(nodeID: DemoCrew.camp, paired: true)

        // Taylor selected by default: the member every LIVE/no-GPS
        // signal/Find screenshot is built around.
        graph.core.crew.selectNode(DemoCrew.taylor)

        location.setFix(world.phoneFix)
        heading.setHeading(world.phoneHeading)

        // Crew pairing, CAMP and the phone's own fix/heading are all
        // live now — safe for `RootView`'s `-FireflyDemoScreen` handling
        // to call `withdrawPhoneFix()`/`startFindOnTaylor()` (this
        // property's own doc comment). Flipped BEFORE the remaining
        // timeline (the incoming text, the two DMs) so a screenshot of
        // Radar/Find never has to wait out the Thread-only tail of the
        // script.
        isStarted = true

        // The incoming text — after `inbox.observe()` above, so it
        // lands in Taylor's thread rather than being silently missed.
        client.injectIncomingText(world.incomingFromTaylor)

        // A second RSSI sample for Taylor: same identity/position, a
        // stronger reading a few seconds "later" — gives
        // `ff_crew_rssi_trend` an actual RISING trend for the no-GPS
        // signal view instead of a flat first-ever sample.
        try? await Task.sleep(nanoseconds: 150_000_000)
        client.injectNodeUpdate(world.taylorSecondRSSI)

        await sendDemoThreadMessages()
    }

    /// Two outgoing DMs to Taylor through the REAL compose path
    /// (`ThreadViewModel.sendCompose()` — the same call the compose
    /// bar's send button makes), so the Thread screenshot shows a real
    /// WAITING -> SENT -> DELIVERED and a real WAITING -> SENT -> NO ACK,
    /// not two rows invented directly in the feed.
    private func sendDemoThreadMessages() async {
        guard let thread = taylorThread else { return }
        thread.composeText = "on my way, see you at the tower"
        await thread.sendCompose()
        try? await Task.sleep(nanoseconds: 1_200_000_000)
        thread.composeText = "you getting signal down there?"
        await thread.sendCompose()
    }

    /// `DemoMeshtasticClient.onSendPrivate`'s consumer: decodes the
    /// outgoing frame (only `FireflyModel` may — `FireflyPacket` is
    /// this module's, not `FireflyMesh`'s), and if it is a FIND PING,
    /// schedules a scripted PONG so the "Find mode" screenshot has a
    /// real reply to show instead of a spinner that never resolves.
    private func replyToFindPingIfNeeded(payload: Data, destination: UInt32, wantAck: Bool) {
        guard let decoded = FireflyPacket.decode(payload), case .ping(let nonce) = decoded else { return }
        let client = client
        Task {
            try? await Task.sleep(nanoseconds: 500_000_000)
            guard let reply = FireflyPacket.pong(nonce: nonce, rssiDbm: -55, snrDb: 6.0).encode() else { return }
            client.injectIncomingPrivate(IncomingPrivate(
                from: destination, to: DemoCrew.jake, channel: 0, packetID: 900_500 &+ nonce,
                payload: reply, rxTime: Date(), rssiDbm: -55, snrDb: 6.0, direct: true))
        }
    }

    /// `-FireflyDemoScreen radar-signal`'s own request: withdraw the
    /// phone's fix so `ff_radar_compute` honestly falls back to
    /// RADAR_SIGNAL for the selected member (`my_pos_ok == false` —
    /// `firmware/core/src/ff_radar.c`) instead of any special-cased
    /// "signal mode" flag. Restores with `restorePhoneFix()`.
    public func withdrawPhoneFix() { location.setFix(nil) }
    public func restorePhoneFix() { location.setFix(world.phoneFix) }

    /// `-FireflyDemoScreen find`'s own request: select Taylor (already
    /// the default selection, restated here so a screenshot script
    /// never depends on ordering) and start a real FIND session over
    /// the real `FindBridge`.
    public func startFindOnTaylor() {
        graph.core.crew.selectNode(DemoCrew.taylor)
        radar.startFindOnSelection()
    }

    public func selectMember(_ nodeID: UInt32) {
        graph.core.crew.selectNode(nodeID)
    }

    /// See `isStarted`'s own doc comment: awaited by `RootView` before
    /// it calls anything else on this type.
    public func waitUntilStarted() async {
        while !isStarted {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }
}
