//
//  AppGraph.swift — the live object graph: ONE client, ONE set of C
//  contexts, ONE GPS uplink, and the view models built on top of them.
//
//  `AppDependencies` (S5) answers "which implementations"; this answers
//  "how many, owned by whom, subscribed when". Both halves are the
//  composition root A01's "Dependency injection" section describes —
//  constructor injection, one composition root, no service locator and
//  no singletons — and keeping them as two types is what lets a test
//  hand the graph a stub `AppDependencies` and get a real graph over it.
//
//  Why this type has to exist at all: before it, `FireflyApp.init` and
//  `RadarView.init()` EACH called `AppDependencies.current()`, so
//  showing the Radar destination built a second `MeshtasticClient` over
//  a second `BLETransport` — two radios' worth of state for one radio,
//  with the screen observing the one that was never connected. A graph
//  constructed once and handed down is the fix, and the reason nothing
//  below this line may call `.current()` for itself.
//
import FireflyMesh
import Foundation

/// The live graph. `@MainActor` because it owns the `ff_*` contexts
/// (through `CoreStore`) and every view model, all of which live in
/// that one isolation domain per A01's threading model.
@MainActor
public final class AppGraph {
    public let dependencies: AppDependencies
    /// The single owner of every `ff_*` context in the process.
    public let core = CoreStore()
    /// `InboxProviding`, backed by the C core through M3's own
    /// `PersistingInboxProvider` wrapper — never `InMemoryInboxStore`,
    /// which is test-only from here on, and never the bare
    /// `CoreInboxProvider` this used to be: every write flows through
    /// `PersistingInboxProvider` first, so `history` (below) can never
    /// drift from what the live ring actually holds.
    public let inboxProvider: any InboxProviding
    /// M3 — the durable half of message history (docs/specs/
    /// A01-companion-app.md, M3 + Persistence). Owned here, not by
    /// `inboxProvider` alone, because `flushPersistedOutbox()` (below)
    /// needs to read it directly (`pendingOutbox`), and because
    /// `AppGraph.init` reads from it (`loadAllForRestore()`) before
    /// `inboxProvider` — the wrapper — even exists.
    private let historyStore: HistoryStore
    /// PR #281 review, BLOCKING 1: the id sources `makeInboxViewModel()`
    /// hands to `InboxViewModel` (which hands `outboxIDGenerator` on to
    /// every `ThreadViewModel` it opens) — `.shared` by default (every
    /// real launch), overridable ONLY so a test can inject a fresh
    /// instance that behaves exactly like a genuinely independent
    /// process's own `.shared` would, to prove two such lifetimes over
    /// the SAME `historyStore` never collide (`OutboxIDGenerator`'s own
    /// doc comment, `ThreadViewModel.swift`). `init` below seeds
    /// whichever instance it was handed — `.shared` or an override —
    /// from `historyStore`, every launch, before anything can mint a
    /// live id from it (`seedIDGenerators(from:store:outbox:inbound:)`).
    private let outboxIDGenerator: OutboxIDGenerator
    private let inboundFeedIDGenerator: InboundFeedIDGenerator
    /// Portnum 269 (FLARE, and FIND's PING).
    public let packetSender: MeshFireflyPacketSender
    /// M2: the one place a crew member is paired/unpaired/renamed —
    /// writes both `core.crew` (live) and `dependencies.crewPairingStore`
    /// (persisted) together, so Connect's Nearby section and the Crew
    /// section in More can never drift from each other or from what
    /// Radar/Inbox render. See `CrewPairingStore.swift`.
    public let crewPairing: CrewPairingController
    /// A02 slice C — auto crew membership (`CrewMembershipEngine`).
    /// Exposed here because slice B's Crew page and Start screen read it
    /// through `CrewMembershipProviding` — `CoreStore.membership` sees
    /// only the gate half of the same object.
    public let crewMembership: CrewMembershipEngine
    /// A02 §4.2 — the phone's own crew profile (code + human name), and
    /// the ONE instance of it in the process: `FireflyApp` hands this
    /// same object to `CrewController` rather than constructing a second
    /// `CrewProfileStore`, so "which crew am I on" has a single answer
    /// that the Crew page and `crewMembership` below cannot disagree
    /// about.
    public let crewProfileStore: any CrewProfileStoring
    /// "app: festpack from fest-almanac + Lineup" — the Lineup screen's
    /// data source. Demo mode (`dependencies.client is DemoMeshtasticClient`
    /// — the same downcast `FireflyApp.init` uses to recover its own
    /// `DemoRunner`, so there is exactly ONE place that decision is made
    /// twice over rather than a second, independent flag that could
    /// drift from it) gets `DemoFestpackProvider` (Firefly Fields,
    /// bundle-only, per the owner's decision); every other composition
    /// — `.stub()` included, since network access has nothing to do
    /// with whether there is a radio to talk to — gets the real
    /// `AlmanacFestpackProvider`.
    public let festpack: any FestpackProviding
    /// "app: Lineup by-stage grid, day pills, My picks" — superseded
    /// `starredArtists` (`StarredArtistsStoring`, artist-keyed); picks
    /// are per-SET now, see `PicksStore`'s own doc comment.
    public let picks: any PicksStoring
    private let uplink: PhoneGPSUplink

    private var privateObservation: Task<Void, Never>?
    private var tickLoop: Task<Void, Never>?
    /// M3 — flushes `historyStore`'s persisted WAITING items on the
    /// link's next not-ready -> ready edge. See
    /// `observeHistoryOutboxFlush()`'s own doc comment for why this has
    /// to be a graph-level subscription rather than left to whichever
    /// `ThreadViewModel` (if any) happens to be open.
    private var historyOutboxFlushObservation: Task<Void, Never>?
    private var started = false
    /// M2: true once `autoConnectToLastKnownPeripheral()` has been
    /// considered, ever — set on the FIRST `start()` only, deliberately
    /// never reset by `stop()`. Without this, `handleScenePhaseChange
    /// (.foreground)`'s own `start()` call would auto-connect on EVERY
    /// foreground resume, including one that just followed a
    /// `backgroundConnectEnabled == false` disconnect — silently undoing
    /// "off means off, the user taps CONNECT" the moment they glance at
    /// another app and back. This flag confines the auto-connect to true
    /// process launch, matching the M2 task's own wording ("auto-
    /// connecting to it at launch").
    private var hasAttemptedLaunchAutoConnect = false
    /// Handed a decoded PONG so FIND's replies list and haptics update.
    /// Set when `makeRadarViewModel` builds one; nil before that, which
    /// is why a PONG arriving with no Radar on screen is dropped rather
    /// than queued — S29's FIND is a live, on-face activity and a reply
    /// to a session nobody is watching has nothing to update.
    private weak var radar: RadarViewModel?
    /// Whether Radar's pump was running the moment `stop()` tore the
    /// graph down — the one thing `start()` needs in order to RESTORE
    /// it rather than wake it (see `start()`'s own comment). `false`
    /// before any `stop()` has ever run: a launch `start()` must not
    /// start a pump for a screen that may not even be selected —
    /// `makeRadarViewModel()` starts it once for the graph's own
    /// factory contract, and `RootView.applyFindLifecycle()` is what
    /// decides whether it keeps running from the first frame on.
    private var radarWasObservingAtStop = false

    // MARK: - M2: inbound FLARE/RALLY/STATUS + PING auto-reply
    // (docs/specs/A01-companion-app.md M2; see `AppGraph+M2Protocol.swift`
    // for the methods that use these — split into its own file so this
    // shared file's own diff stays small, three other M2 slices touching
    // it in the same worktree).

    /// The inbound-FLARE takeover's whole state — one per process, like
    /// every other `ff_*`-backed view model this graph owns.
    public let flareTakeover: FlareTakeoverViewModel
    /// `UNNotificationSending` by default (the live graph) — local
    /// notifications for a backgrounded FLARE/text (task point 4).
    /// Injectable so a test never has to touch the real
    /// `UNUserNotificationCenter` (`UNNotificationSending`'s own doc
    /// comment on why that matters under bare `swift test`).
    /// `public` as of A03: the Diagnostics screen reads the
    /// notification AUTHORIZATION through this same instance (§3.10's
    /// "Firefly can't alert you" row), rather than constructing a second
    /// seam of its own that could disagree with the one that posts.
    public let notifications: any NotificationSending
    /// Whether the app is in the foreground right now — `FireflyApp`'s
    /// `ScenePhase` observation is the one caller (`setForegrounded(_:)`).
    ///
    /// A03 §3.2 — starts **`false`**, and this is a correction, not a
    /// preference. It used to start `true` on the reasoning that "the
    /// app IS foregrounded when its composition root is built" — which
    /// is exactly what a CoreBluetooth background relaunch is not, and
    /// `.onChange(of: scenePhase)` does not fire for an initial value,
    /// so nothing corrected it. Any launch that begins in the background
    /// left this graph believing someone was looking at the screen:
    /// `handleInboundFlare` took the TAKEOVER branch and rendered a
    /// full-screen view to nobody, and
    /// `observeIncomingTextsForNotifications` skipped every arriving
    /// message. Zero notifications on exactly the path notifications
    /// exist for (audit 2.3.10).
    ///
    /// `false` is the honest default and it fails safe in the direction
    /// that matters: an inbound FLARE on a path where nothing has told
    /// us we are visible posts a notification rather than rendering a
    /// takeover to an empty screen. `FireflyApp` seeds the real value
    /// from the initial `scenePhase` at scene attach.
    /// `public private(set)`: every WRITE still goes through
    /// `setForegrounded(_:)` (one caller, `FireflyApp`'s scene-phase
    /// observer), but A03_AC8 has to be able to READ the value without
    /// `@testable` — "false on construction" is the criterion itself,
    /// not an implementation detail.
    public private(set) var isForegrounded = false
    /// The phone's own last known fix, kept live by `observeMyLocation()`
    /// — shared by `FlareTakeoverViewModel`'s bearing/distance and
    /// inbound RALLY's own distance/bearing text.
    var myFix: LocationFix?
    var locationObservation: Task<Void, Never>?
    /// A second, independent `incomingTexts()` subscription purely for
    /// backgrounded-text notifications — see `observeIncomingTextsForNotifications()`.
    var incomingTextNotificationObservation: Task<Void, Never>?
    /// PONG auto-reply's "one reply per nonce" memory.
    var repliedPongNonces = PongReplyDedup()
    /// A03 §3.11.5 — the subscription that notices the first foreground
    /// `.ready`, which is the moment permission is asked.
    var notificationPermissionObservation: Task<Void, Never>?
    /// Once per process, never reset by `stop()`: a second prompt is
    /// something iOS would not show anyway.
    private var hasRequestedNotificationAuthorization = false
    /// A03 §3.11.3 — where a tapped notification (or a `firefly://` URL)
    /// wants to land. The graph OWNS it; `RootView` is what can actually
    /// act on it, since only it owns tab selection.
    public let deepLinks = DeepLinkRouter()

    /// `FireflyApp.init()` alone passes `true` — see
    /// `autoConnectToLastKnownPeripheral()`'s own doc comment for why
    /// this needs to be an explicit opt-in rather than `isRunningUnderXCTest`
    /// alone: `AppGraphTests` constructs `AppGraph` directly and also
    /// runs under XCTest, but legitimately wants launch auto-connect to
    /// behave normally under test.
    private let skipLaunchAutoConnectUnderXCTest: Bool

    /// `historyStore:` — M3's own override seam, `nil` by default. When
    /// `nil`, the store is picked the same way `.stub()`/`.demo()`/
    /// `.live()` already distinguish themselves elsewhere in this file:
    /// `dependencies.store is InMemorySettingsStore` is true for both
    /// `.stub()` and every `.demo()`/`.demoBundle()` composition (and
    /// for nothing `.live()` ever builds), so it doubles as "is this a
    /// disposable stack" without a second flag to keep in sync — a
    /// disposable stack gets a disposable, in-memory history store,
    /// matching M3's own demo-isolation rule ("Demo doesn't persist
    /// across launches: in-memory store only") for free. Explicit
    /// callers use this to inject a store that already has rows in it:
    /// `DemoRunner`'s own `-FireflyDemoRestored` seeding does exactly
    /// that (an in-memory store, pre-populated, so the SAME restore code
    /// path a real relaunch takes is what renders it), and so does any
    /// test that wants to simulate "two launches sharing one store".
    public init(dependencies: AppDependencies = .current(), notifications: any NotificationSending = UNNotificationSending(),
                skipLaunchAutoConnectUnderXCTest: Bool = false, historyStore: HistoryStore? = nil,
                crewProfileStore: (any CrewProfileStoring)? = nil,
                outboxIDGenerator: OutboxIDGenerator = .shared, inboundFeedIDGenerator: InboundFeedIDGenerator = .shared) {
        self.dependencies = dependencies
        self.notifications = notifications
        self.skipLaunchAutoConnectUnderXCTest = skipLaunchAutoConnectUnderXCTest
        self.historyStore = historyStore ?? (dependencies.store is InMemorySettingsStore ? .inMemory() : .live())
        self.crewProfileStore = crewProfileStore ?? Self.makeCrewProfileStore(dependencies: dependencies)
        self.outboxIDGenerator = outboxIDGenerator
        self.inboundFeedIDGenerator = inboundFeedIDGenerator
        let rawInboxProvider = CoreInboxProvider(inbox: core.inbox, crew: core.crew)
        // M3 — reseed the live ring from storage NOW, before
        // `PersistingInboxProvider` even exists and before ANYTHING can
        // observe a client: pushed straight into the RAW provider so
        // restoring a message is never itself treated as new traffic to
        // re-persist (`PersistingInboxProvider`'s own header comment).
        // Mirrors `CrewPairingRestorer.restore`'s identical ordering
        // rule a few lines down, for the identical reason: a
        // want_config replay's first live event must never race a
        // still-in-progress restore.
        //
        // PR #281 review, BLOCKING 1: `seedIDGenerators` runs BEFORE
        // `HistoryRestorer.restore` for the identical reason — both read
        // `allHistory`, computed exactly once here, but only the
        // generator seeding must land before restore's own `push`es,
        // since neither generator is used by `restore` itself (it
        // replays each message's own already-assigned id, never a fresh
        // one) — ordered first anyway so that even the FIRST live id
        // this process could ever mint, however soon after `init`
        // returns, is already past everything `historyStore` holds.
        let allHistory = self.historyStore.loadAllForRestore()
        Self.seedIDGenerators(from: allHistory, store: self.historyStore,
                               outbox: self.outboxIDGenerator, inbound: self.inboundFeedIDGenerator)
        let restoredMessageIDs = HistoryRestorer.restore(allHistory, into: rawInboxProvider)
        self.inboxProvider = PersistingInboxProvider(wrapping: rawInboxProvider, history: self.historyStore,
                                                      restoredMessageIDs: restoredMessageIDs)
        self.packetSender = MeshFireflyPacketSender(client: dependencies.client)
        self.flareTakeover = FlareTakeoverViewModel(crew: self.core.crew)
        self.crewPairing = CrewPairingController(crew: core.crew, store: dependencies.crewPairingStore)
        self.crewMembership = CrewMembershipEngine(pairing: self.crewPairing,
                                                    store: dependencies.crewLocalStateStore,
                                                    client: dependencies.client)
        let isDemo = dependencies.client is DemoMeshtasticClient
        self.festpack = isDemo
            ? DemoFestpackProvider()
            : AlmanacFestpackProvider(settings: dependencies.store)
        // "app: automatic almanac refresh + festival picker" — demo
        // mode's picks live under their own fixed namespace (Firefly
        // Fields is never selectable through the real picker, and
        // `dependencies.store` is a fresh, in-memory store per demo
        // launch anyway — see `PicksStore`'s own doc comment for why a
        // dynamic namespace closure exists at all); every other
        // composition (`.stub()` included, same reasoning `festpack`
        // above already states) resolves the namespace from settings on
        // every read, so a Settings festival-picker selection made
        // mid-session is picked up immediately.
        self.picks = isDemo
            ? PicksStore(store: dependencies.store, namespace: { "firefly-fields-2026" })
            : PicksStore(store: dependencies.store, namespace: { [store = dependencies.store] in store.festivalNamespace() })
        let client = dependencies.client
        self.uplink = PhoneGPSUplink(
            location: dependencies.location,
            settings: dependencies.store,
            sink: MeshPositionSink(client: client),
            // Read per fix, never captured once: before the handshake
            // there is no node to address, and a fix arriving then is
            // dropped rather than sent to a guessed destination.
            destinationNodeNum: { client.connectedNodeNum })
        // Only safe now: every stored property above is set, so `self`
        // may finally be captured (`setCurrentFix`'s own doc comment).
        self.flareTakeover.setCurrentFix { [weak self] in self?.myFix }
        // M2: replay the persisted paired list onto `core.crew` NOW —
        // before `start()` ever subscribes `core` to the client's
        // `nodeUpdates()` stream (`start()` is a separate, later call;
        // nothing above this line touches the client's streams either).
        // A want_config replay's `CoreStore.apply(nodeUpdate:)` ->
        // `crew.setIdentity` must never be the first thing to see a
        // reconnecting member's slot — restoring PAIRED (and the
        // member's persisted colour) here first is what keeps that flag
        // from ever reading as lost for even one frame.
        // `CrewPairingRestorer`'s own doc comment.
        CrewPairingRestorer.restore(from: dependencies.crewPairingStore, into: core.crew)
        // A02 AC13 — install the membership gate BEFORE `start()` can
        // subscribe `core` to `nodeUpdates()`, and after the restore
        // above, for the same ordering reason: the gate answers "is this
        // node already crew?" off the live roster, so the roster has to
        // be restored first or the first want_config replay would find
        // every returning member unpaired.
        core.membership = crewMembership
        // A02 §4.1/§4.2, PR #313 review — THE production call site for
        // `CrewMembershipEngine.configure(crew:)`. Without this the
        // engine stays `.noCrew` for the whole life of the process no
        // matter what the user started or joined, `admits(_:)` refuses
        // everybody at clause 2, and slice C's entire auto-membership
        // rule is inert in the shipped app (it was — nothing called
        // `configure` outside demo mode until this line).
        //
        // Here, in `init`, for the same ordering reason `core.membership`
        // above is: the gate must already know which crew it is gating
        // for before `start()` can subscribe `core` to `nodeUpdates()`,
        // or the first packets of a session would be judged against
        // `.noCrew` and refused. The channel INDEX is resolved
        // separately and asynchronously (`configure` -> `.resolving` ->
        // `resolveCrewChannelIndex`), which is correct: at `init` there
        // is no link yet, an unread channel table is `.resolving`, not
        // "your puck isn't on this crew's channel", and `observe()` re-
        // resolves on every `.ready` — i.e. after every want_config.
        syncCrewMembershipWithProfile()
    }

    /// Point `crewMembership` at whatever crew this phone is actually on
    /// — called at `init` and again by `CrewController` after every
    /// Start, Join, switch and Leave (`FireflyApp.init` wires that
    /// callback; `CrewController.onProfileChanged`).
    ///
    /// Reads the profile back out of the store rather than taking one as
    /// an argument, deliberately: the store is the thing that persists,
    /// so a caller can never hand this a crew that a relaunch would
    /// disagree with. Leave clears the profile, so Leave lands here as
    /// `configure(crew: nil)` with no separate "clear" path to forget.
    ///
    /// The PSK is DERIVED from the code (`CrewKey.psk(for:)`), never
    /// stored alongside it and never read off the radio: §4.2 resolves
    /// the channel index by name AND key, and a key taken from the radio
    /// would make that comparison compare the radio with itself.
    @discardableResult
    public func syncCrewMembershipWithProfile() -> CrewChannelIdentity? {
        guard let profile = crewProfileStore.load(),
              let code = try? CrewCode.parse(profile.code) else {
            crewMembership.configure(crew: nil)
            return nil
        }
        let identity = CrewChannelIdentity(code: code.canonical, psk: CrewKey.psk(for: code))
        crewMembership.configure(crew: identity)
        return identity
    }

    /// The crew profile store for this composition. Demo mode gets an
    /// in-memory one, pre-seeded ONLY for `-FireflyDemoScreen crew`
    /// (`DemoCrew.profile`) — so the demo crew cannot persist (there is
    /// nothing behind it to persist to) and cannot appear outside a
    /// process that is genuinely running the demo world, since
    /// `AppDependencies.current()` only ever builds a
    /// `DemoMeshtasticClient` inside `#if targetEnvironment(simulator)`
    /// and behind `DemoLaunch.isRequested()`. A stray
    /// `-FireflyDemoScreen crew` on a real device reaches neither.
    ///
    /// Every other disposable stack (`.stub()`, tests) gets a plain
    /// empty in-memory store, and only a real `.live()` composition
    /// touches `UserDefaults` — the same `dependencies.store is
    /// InMemorySettingsStore` tell `historyStore` above already uses.
    private static func makeCrewProfileStore(dependencies: AppDependencies) -> any CrewProfileStoring {
        guard dependencies.client is DemoMeshtasticClient else {
            return dependencies.store is InMemorySettingsStore ? InMemoryCrewProfileStore() : CrewProfileStore()
        }
        let store = InMemoryCrewProfileStore()
        if DemoLaunch.requestedScreen() == "crew" { store.save(DemoCrew.profile) }
        return store
    }

    /// `FireflyApp`'s `ScenePhase` observation calls this — the one
    /// source of truth `handleInboundFlare`/the notification path below
    /// read to decide "takeover, or a local notification instead".
    public func setForegrounded(_ active: Bool) {
        isForegrounded = active
        guard active else { return }
        // A03 §3.11.5, the S1a review's own leftover: the ask is armed on
        // the first `.ready` seen WHILE FOREGROUNDED — but a link that
        // reached `.ready` while the app was backgrounded does not
        // re-publish `.ready` when the user comes back (`EventHub` is
        // multicast, never replayed — S1). That is precisely the
        // restored-session case §3.1 exists for: the phone was relaunched
        // into the background, adopted the session, handshook, and the
        // user opens the app an hour later to a link that has been
        // `.ready` the whole time. Without this, that user is never asked
        // for notification permission at all, and the first FLARE of the
        // festival is lost to the very bug audit 2.3.11 describes.
        //
        // Foregrounding is therefore the second trigger, and it reads a
        // state this graph actually OBSERVED (`lastObservedLinkState`),
        // never an assumption about what the link is probably doing.
        guard lastObservedLinkState == .ready else { return }
        Task { await requestNotificationAuthorizationIfNeeded() }
    }

    /// The most recent `LinkState` `observeLinkForNotificationPermission()`
    /// actually saw. `nil` until the link publishes anything — an honest
    /// "nothing observed yet", not a fabricated `.disconnected`.
    private var lastObservedLinkState: LinkState?

    /// A03 §3.11.5 — asked in the foreground, at a moment that can
    /// actually answer.
    ///
    /// The first time the link reaches `.ready` WHILE THE APP IS IN THE
    /// FOREGROUND, and never from a posting path. The bug this closes
    /// (audit 2.3.11) is worth restating because it is not obvious:
    /// authorization used to be requested lazily, on first need — and
    /// the only callers were the backgrounded branches, so the first
    /// FLARE of the festival asked for permission while iOS could not
    /// present a prompt, read back `.notDetermined`, and dropped the
    /// alert. The first notification of the festival was ALWAYS lost.
    ///
    /// Once per process (`hasRequestedNotificationAuthorization`): iOS
    /// only ever shows the system prompt once anyway, and asking again
    /// after a "no" is both useless and rude.
    ///
    /// §3.11.5's own pre-prompt — a one-line explanation with a single
    /// button on the Connect screen — is S2: it is Connect-screen UI,
    /// and the crew Start/Join work is in that file concurrently. What
    /// ships here is the TIMING fix, which is the half that decides
    /// whether an alert is ever delivered at all.
    func observeLinkForNotificationPermission() {
        guard notificationPermissionObservation == nil else { return }
        let states = dependencies.client.linkState()
        notificationPermissionObservation = Task { [weak self] in
            for await state in states {
                guard let self else { return }
                // Recorded FIRST, for every state, because
                // `setForegrounded(_:)` reads it as the second trigger
                // (its own doc comment) — a `.ready` this loop declines
                // to act on here is exactly the one a later foreground
                // transition has to act on.
                self.lastObservedLinkState = state
                guard state == .ready, self.isForegrounded else { continue }
                await self.requestNotificationAuthorizationIfNeeded()
            }
        }
    }

    /// Public so the Connect screen's own button (S2) can call exactly
    /// this, rather than growing a second path to the same prompt.
    public func requestNotificationAuthorizationIfNeeded() async {
        guard !hasRequestedNotificationAuthorization else { return }
        guard Self.shouldRequestNotificationAuthorization(isDemoStack: isDemoStack) else { return }
        hasRequestedNotificationAuthorization = true
        await notifications.requestAuthorization()
    }

    /// REVIEW FIX (PR #310) — **never in the demo stack.**
    ///
    /// Found by running the UI smoke test: the demo client reaches
    /// `.ready` on its own a moment after launch, which is exactly the
    /// trigger §3.11.5 defines, so `-FireflyDemo` raised a real
    /// SpringBoard "Firefly Would Like to Send You Notifications" alert
    /// over the app. XCUITest's interruption handler dismissed it and
    /// retried, but the alert's own dimming layer ate the next tab-bar
    /// tap and `testDemoSmokeTapsThroughAllScreens` sat on
    /// `Screen.Connect` for the full 60 s waiting for Radar. (The tell in
    /// the failure attachments is `AdditionalDimmingOverlay` — a
    /// SpringBoard alert's scrim — present in the snapshot alongside
    /// `Screen.Connect`.)
    ///
    /// It is the right product behaviour independently of the test: the
    /// demo stack has no radio and no crew, nothing in it can ever post
    /// a notification, and a system permission prompt is exactly the
    /// thing that must not appear in the middle of a scripted demo or a
    /// marketing screenshot (S20's own premise).
    ///
    /// The two existing XCTest signals do not cover this: both
    /// `isRunningUnderXCTest` and `isXCTestRuntimeLoaded` are false in
    /// the app-under-test of a UI test — that process links no XCTest
    /// runtime and carries no `XCTestConfigurationFilePath`, as
    /// `shouldAutoRefreshFestpack`'s own doc comment says. The demo
    /// stack is the signal that is actually true here.
    public nonisolated static func shouldRequestNotificationAuthorization(isDemoStack: Bool) -> Bool {
        !isDemoStack
    }

    /// Whether this graph was built over the scripted demo client
    /// (`-FireflyDemo`/`FIREFLY_DEMO=1`, inside
    /// `#if targetEnvironment(simulator)` — `AppDependencies.current()`).
    /// Read from the client the composition root actually handed us
    /// rather than from a second flag that could disagree with it.
    public var isDemoStack: Bool { dependencies.client is DemoMeshtasticClient }

    /// Idempotent, the same convention every `observe()` in this app
    /// follows. Subscribes `CoreStore` to the client's streams, starts
    /// the phone-GPS uplink, starts the ack-timeout tick, and subscribes
    /// to portnum 269.
    ///
    /// The GPS uplink is started unconditionally and gates ITSELF on
    /// `SettingsKey.locationSharingEnabled` per fix
    /// (`PhoneGPSUplinkPolicy.shouldPush`'s `enabled:`), rather than
    /// being started and stopped as the setting is toggled: the setting
    /// can change while a fix is already in flight, and one gate
    /// evaluated at the moment of each push is the only version of this
    /// that cannot leak a position after the user turned sharing off.
    ///
    /// `async` for one specific reason: `PhoneGPSUplink` is an actor, so
    /// its `start()` — which is where `location.fixes()` is actually
    /// subscribed — can only be reached with an `await`. Firing that off
    /// in a detached `Task` instead would mean the uplink's subscription
    /// races the first fix, and `EventHub` is multicast, NOT replayed
    /// (S1): a fix yielded before the subscription registers is gone.
    /// Awaiting it here makes "the graph has started" mean "every
    /// subscription is live", which is the only version of this that a
    /// test can assert on and a user can rely on.
    ///
    /// A03 §3.1 — `attemptLaunchAutoConnect` exists for the ONE caller
    /// that must not connect: a CoreBluetooth background relaunch.
    /// §3.1's rule is "neither path may call `connect()` — restoration
    /// must be allowed to ADOPT the session rather than race a fresh
    /// connect", and `autoConnectToLastKnownPeripheral()` below is a
    /// `connect()`, one `start()` hop removed. Racing it against an
    /// in-flight `willRestoreState` adoption is the §1.2 teardown this
    /// whole slice exists to prevent: `BLETransport
    /// .performConnectSequence()` assigns `peripheral = ` whatever
    /// `retrievePeripherals(withIdentifiers:)` hands back, and releasing
    /// the restored object implicitly calls
    /// `cancelPeripheralConnection(_:)`.
    ///
    /// The attempt is NOT dropped, only deferred: it runs on whichever
    /// `start()` first has a scene behind it (`FireflyApp`'s own
    /// `.task`, or `handleScenePhaseChange(.foreground)`), which is
    /// exactly where it ran before S1b added a launch hook at all.
    /// Hence the `defer` — `hasAttemptedLaunchAutoConnect` still confines
    /// it to one attempt per process, so the later call is a no-op
    /// whenever an earlier one already ran.
    public func start(attemptLaunchAutoConnect: Bool = true) async {
        defer {
            if attemptLaunchAutoConnect { attemptLaunchAutoConnectIfNeeded() }
        }
        Self.log("start() called (started=\(started), attemptLaunchAutoConnect=\(attemptLaunchAutoConnect))")
        guard !started else {
            Self.log("start(): already started — no-op")
            return
        }
        started = true
        // `routeDeliveriesToInbox: false` — the view-model path owns the
        // feed's outbox id space here. See
        // `CoreStore.observe(client:routeDeliveriesToInbox:)`.
        core.observe(client: dependencies.client, routeDeliveriesToInbox: false)
        // A02 AC14 — re-resolve the crew's channel index on every
        // reconnect. Subscribed here, alongside every other stream this
        // graph owns, rather than inside the engine's init: a
        // subscription that outlives `stop()` is the leak `stopObserving`
        // exists to prevent.
        crewMembership.observe()
        // `observe()` only re-resolves when a NEW `.ready` arrives, and
        // `EventHub` is multicast, not replayed (S1) — so a graph that
        // was stopped and started again while the link stayed up would
        // otherwise keep an index resolved against the PREVIOUS session.
        // Re-reading the table once here costs one admin round trip and
        // removes the only path by which a cached index outlives the
        // link it was read from.
        crewMembership.resolveCrewChannelIndex()
        // Paired with `stop()`'s `radar?.stopObserving()` — without this,
        // backgrounding with background-connect off would stop Radar's
        // recompute loop permanently and coming back to the foreground
        // would show a frozen Radar.
        //
        // RESTORE, not "start unconditionally" (PR #298 review): since
        // the Find tab landed, Radar's pump is only supposed to run
        // while Find's Radar segment is the thing on screen
        // (`FindLifecycle`, docs/specs/A01-companion-app.md
        // "Navigation"). An unconditional `observe()` here woke that
        // 1 Hz `ff_radar_compute` pump on EVERY foreground, including
        // one that resumed onto Find's Map segment, Inbox or Lineup —
        // and nothing off-screen ever stopped it again, which is the
        // same off-screen leak `MapTabView`'s own `isOnScreen` guard
        // exists to close. `stop()` records whether the pump was
        // actually running when it tore things down, so this puts back
        // exactly what was there and never more than that. Also why
        // this cannot simply be re-applied from the UI layer on
        // `.active`: `start()` runs in its own `Task`, so a synchronous
        // `scenePhase` handler in a view would always lose that race.
        if radarWasObservingAtStop { radar?.observe() }
        observePrivatePackets()
        observeMyLocation()
        observeIncomingTextsForNotifications()
        observeLinkForNotificationPermission()
        observeHistoryOutboxFlush()
        // A03 §3.1 — the client attaches to the transport HERE, after
        // every subscription above is live and BEFORE any connect.
        // Two things depend on the position of this line: a restored
        // session's `.ready` needs a listener (and `beginListening()`
        // additionally ASKS the transport whether the link is already up,
        // for the ordering where the adoption won the race), and the
        // handshake it may kick off publishes through `CoreStore`'s and
        // this graph's own subscriptions — which is why it cannot move
        // above `core.observe(client:)`.
        await dependencies.client.beginListening()
        // A03 §1.10 — categories (and their actions) must be registered
        // on EVERY launch, background relaunches included, or a delivered
        // notification's category is unknown to the system and its
        // action never appears. Idempotent; not awaited, because nothing
        // below depends on it.
        Task { [notifications] in await notifications.registerCategories() }
        Self.log("start(): awaiting uplink.start()")
        await uplink.start()
        Self.log("start(): uplink.start() returned")
        tickLoop = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self else { return }
                // `ff_feed_expire_pending_acks`, the ACK-TIMEOUT half of
                // NO_ACK — it has no event to arrive on, so something has
                // to call it, exactly as `ff_shell_tick` does on the puck
                // (`CoreStore.tick(nowMs:)`'s own doc comment).
                self.core.tick(nowMs: FireflyClock.nowMillis())
                // The FLARE takeover's own auto-dismiss (S10: "Auto-end
                // at dur") — same tick-driven shape, same loop.
                self.flareTakeover.tick()
            }
        }
        // "app: automatic almanac refresh" (owner ask #1, 2026-09-13) —
        // covers genuine launch AND a restart after `stop()` (the
        // `backgroundConnectEnabled == false` case); the foreground
        // resume that does NOT restart the graph is covered separately
        // by `handleScenePhaseChange(.foreground)`'s own call, since
        // this `start()` body would not even run for it.
        triggerFestpackAutoRefresh()
        Self.log("start() completed — every subscription is live")
    }

    /// Fires `festpack.refreshIfNeeded()` as its own `Task`, never
    /// awaited inline — matching `autoConnectToLastKnownPeripheral()`'s
    /// own reasoning just above: a slow or absent network must not hold
    /// up `start()`'s own completion (the tick loop, the private-packet
    /// reader), and `refreshIfNeeded()`'s own throttle/staleness policy
    /// already makes most calls a same-actor no-op.
    private func triggerFestpackAutoRefresh() {
        guard Self.shouldAutoRefreshFestpack(isRunningUnderXCTest: Self.isXCTestRuntimeLoaded) else {
            Self.log("triggerFestpackAutoRefresh(): running under XCTest — not fetching")
            return
        }
        Task { [festpack] in
            await festpack.refreshIfNeeded()
        }
    }

    /// The one rule behind the guard above, pure so both branches are
    /// testable without a graph, a network or a clock.
    ///
    /// Review fix: `.stub()` — the stack EVERY unit test composes, and
    /// the one `.current()` hands the iOS Simulator — carries
    /// `StubMeshtasticClient`, not `DemoMeshtasticClient`, so `init`
    /// above builds a real `AlmanacFestpackProvider` over a real
    /// `URLSessionFestpackFetcher` for it. Firing `refreshIfNeeded()`
    /// from `start()` therefore put a live HTTPS GET to
    /// raw.githubusercontent.com inside `swift test`/`xcodebuild test`
    /// on any machine with no warm disk cache — i.e. every CI runner
    /// (measured: it does not reproduce locally precisely BECAUSE the
    /// dev machine's Application Support cache is warm and under the
    /// 6-hour staleness threshold). Unit tests must not depend on the
    /// network; the refresh policy itself stays fully covered by
    /// `AlmanacFestpackProviderTests`, which drives `refreshIfNeeded()`
    /// directly against a stub fetcher.
    ///
    /// Detected via `isXCTestRuntimeLoaded`, NOT the
    /// `XCTestConfigurationFilePath` environment variable
    /// `isRunningUnderXCTest` reads: measured on this toolchain, a bare
    /// `swift test` run does not set that variable at all (the whole
    /// test process environment carries only `SWIFT_TESTING_ENABLED`),
    /// so the existing check reports false in exactly the suite this
    /// gate has to cover. Like the launch auto-connect, this suppresses
    /// nothing in a UI test or a plain Simulator run — the app under
    /// test is its own process with no XCTest runtime in it.
    nonisolated static func shouldAutoRefreshFestpack(isRunningUnderXCTest: Bool) -> Bool {
        !isRunningUnderXCTest
    }

    /// True when the XCTest runtime is loaded into THIS process — the
    /// one signal that holds for both `swift test` (an `xctest` host
    /// process) and `xcodebuild test` (XCTest injected into
    /// `Firefly.app`), and false for a plain app launch, a Simulator
    /// run, and the app-under-test of a UI test, none of which link it.
    ///
    /// Deliberately separate from `isRunningUnderXCTest` above rather
    /// than a fix to it: that property gates
    /// `skipLaunchAutoConnectUnderXCTest`, whose behaviour on the BLE
    /// bench was tuned against exactly what that variable does today,
    /// so widening it changes a different, already-shipped decision and
    /// belongs in its own PR.
    nonisolated static var isXCTestRuntimeLoaded: Bool {
        NSClassFromString("XCTestCase") != nil
    }

    /// Same discipline as `BLETransport.log(_:)`/`MeshtasticClient.log(_:)`:
    /// a raw stderr write, unconditional, so the composition root's own
    /// startup sequence — in particular WHEN (if ever) the launch
    /// auto-connect fires relative to `start()` finishing — is visible
    /// in the same log a `MeshtasticClient`/`BLETransport` capture
    /// already carries, not a separate channel that has to be
    /// cross-referenced by timestamp alone.
    private static func log(_ message: String) {
        let line = "[AppGraph] \(message)\n"
        FileHandle.standardError.write(Data(line.utf8))
    }

    /// True when `XCTestConfigurationFilePath` is set — Xcode's own test
    /// runner sets this for `xcodebuild test` (both an app-hosted UI
    /// test and a logic-test bundle), which is the ONLY case this
    /// property is actually used to gate (`autoConnectToLastKnownPeripheral()`'s
    /// own doc comment on `skipLaunchAutoConnectUnderXCTest` says why it
    /// has to be an explicit opt-in rather than a bare check on this).
    ///
    /// CORRECTION (app: Map subscribes to festpack updates, 2026-09-13
    /// review — this doc comment previously claimed the opposite):
    /// measured on this toolchain, a bare `swift test` process does NOT
    /// set this variable at all — `shouldAutoRefreshFestpack(isRunningUnderXCTest:)`'s
    /// own doc comment states this correctly and is the reason
    /// `isXCTestRuntimeLoaded` (below) exists as a SEPARATE, wider
    /// check: it is the one signal that also catches `swift test`'s own
    /// `AppGraphTests` host process. Do not widen THIS property to
    /// match — it is deliberately narrower, and `isXCTestRuntimeLoaded`'s
    /// own doc comment explains why the two are kept apart rather than
    /// unified into one.
    static var isRunningUnderXCTest: Bool {
        ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
    }

    /// One launch auto-connect attempt per process, wherever `start()`
    /// is first called with a scene behind it. Split out of `start()`'s
    /// body so the background-relaunch caller can opt out of it without
    /// consuming the flag (`start(attemptLaunchAutoConnect:)`'s own doc
    /// comment).
    private func attemptLaunchAutoConnectIfNeeded() {
        guard !hasAttemptedLaunchAutoConnect else { return }
        hasAttemptedLaunchAutoConnect = true
        let remembered = dependencies.store.string(.lastPeripheralID)
        Self.log("start(): considering launch auto-connect — lastPeripheralID=\(remembered ?? "nil")")
        autoConnectToLastKnownPeripheral()
    }

    /// M2 — "remembering the last connected peripheral identifier and
    /// auto-connecting to it at launch" (docs/specs/A01-companion-app.md;
    /// behaviour borrowed from Meshtastic-Apple's own launch-time
    /// auto-connect to the preferred device, re-implemented against this
    /// app's own client seam). Only ever fires anything under the LIVE
    /// stack: `.stub()`/`.demo()` each construct a fresh
    /// `InMemorySettingsStore()` with nothing persisted in it, so
    /// `lastPeripheralID` reads nil there by construction — no auto-
    /// connect noise in a test or the iOS Simulator.
    ///
    /// Fired off as its own `Task`, never awaited inline: a radio that
    /// is not in range yet must not hold up the rest of this method's
    /// own startup (the tick loop, the private-packet reader) — and
    /// `MeshtasticClientProtocol.connect()`'s own retry/timeout already
    /// handles a dead or out-of-range node on its own. `try?` swallows
    /// `MeshtasticClientError.alreadyConnecting` on purpose: if the
    /// Connect screen's own CONNECT button won the race instead, that
    /// attempt is the one that should finish, not this one.
    private func autoConnectToLastKnownPeripheral() {
        if skipLaunchAutoConnectUnderXCTest, Self.isRunningUnderXCTest {
            // Follow-up to the 2026-09-11 bench-reproduced power-cycle
            // investigation (`docs/specs/A01-companion-app.md`, M2):
            // `BLEHardwareTests` constructs and drives its OWN
            // `BLETransport`/`MeshtasticClient` directly, never through
            // this graph — but `xcodebuild test -only-testing:
            // FireflyHardwareTests` still launches `Firefly.app` itself
            // as the test's HOST application, and `FireflyApp`'s own
            // `.task { await graph.start() }` ran this graph's launch
            // auto-connect in that SAME process the whole time: a
            // second, entirely independent `BLETransport` connecting to
            // `SettingsKey.lastPeripheralID`'s remembered peripheral
            // (`Meshtastic_06b0`, on this bench) and racing whatever the
            // test itself was doing over BLE, confusing the test's own
            // log with a second `[AppGraph]`/`[BLETransport]` sequence
            // that has nothing to do with the test. Only
            // `FireflyApp.init()` passes `skipLaunchAutoConnectUnderXCTest:
            // true` (this type's own doc comment) — every test that
            // constructs `AppGraph` directly, including this file's own
            // auto-connect tests, defaults to `false` and is untouched.
            Self.log("autoConnectToLastKnownPeripheral(): running under XCTest (XCTestConfigurationFilePath set) " +
                     "and skipLaunchAutoConnectUnderXCTest is set — not connecting")
            return
        }
        guard dependencies.store.string(.lastPeripheralID) != nil else {
            Self.log("autoConnectToLastKnownPeripheral(): nothing remembered — not connecting")
            return
        }
        Self.log("autoConnectToLastKnownPeripheral(): firing client.connect() as its own Task")
        Task { [dependencies] in
            do {
                try await dependencies.client.connect()
                Self.log("autoConnectToLastKnownPeripheral(): client.connect() returned successfully")
            } catch {
                // `try?` below still swallows this — logged here first so
                // a `.alreadyConnecting` loss to the Connect screen's own
                // manual CONNECT (or any other failure) is visible rather
                // than silently disappearing into the `try?`.
                Self.log("autoConnectToLastKnownPeripheral(): client.connect() threw \(error)")
            }
        }
    }

    // MARK: - A03 §3.1: launch, including the ones nobody can see

    /// Construct the BLE `CBCentralManager` NOW, synchronously, so
    /// CoreBluetooth has a manager carrying the fixed restore identifier
    /// during the launch cycle (§3.1, A03_AC1).
    ///
    /// Two callers, deliberately: the `UIApplicationDelegate` (via
    /// `handleDidFinishLaunching(isForegrounded:)` below — the hook iOS
    /// actually guarantees runs on a background relaunch) and
    /// `FireflyApp.init()` as a second path for the ordering question
    /// SwiftUI does not document. Both are idempotent, so whichever runs
    /// first wins and the other is a no-op.
    ///
    /// It issues no `connect()` and starts no scan — restoration must be
    /// allowed to ADOPT the session rather than race a fresh connect —
    /// and it is a no-op on a stack with no BLE under it at all (the
    /// stub stack, the iOS Simulator, the demo world), which is what
    /// `NodeScanning`'s empty default implementation states.
    public func prepareForRestoration() {
        guard !shouldSkipLaunchWorkUnderXCTest else {
            Self.log("prepareForRestoration(): running under XCTest as an app-hosted test's HOST — not constructing a manager")
            return
        }
        Self.log("prepareForRestoration(): constructing the central manager")
        dependencies.scanner?.prepareForRestoration()
    }

    /// `application(_:didFinishLaunchingWithOptions:)`'s whole body,
    /// where it can actually be tested (the delegate itself is iOS-only
    /// UIKit glue; this is platform-free).
    ///
    /// Order is the point, and it is the order §3.1 states:
    ///
    /// 1. `prepareForRestoration()` — SYNCHRONOUSLY, before this method
    ///    returns to UIKit, because that is when iOS wants the manager to
    ///    exist.
    /// 2. Seed `isForegrounded` from `UIApplication.applicationState`
    ///    (§3.2) — the only signal available during a background
    ///    relaunch, before any scene exists.
    /// 3. Kick `start()`, which is what subscribes `MeshtasticClient` to
    ///    the transport (`beginListening()`) so the restored session has
    ///    a listener at all. Not awaited — it cannot be, from a
    ///    synchronous UIKit callback — and it does not need to be:
    ///    `start()`'s own `started` guard makes it idempotent against the
    ///    scene's `.task`, and `beginListening()` covers BOTH orderings
    ///    of itself against the restore (its own doc comment).
    ///    `attemptLaunchAutoConnect: isForegrounded` is §3.1's "neither
    ///    path may call `connect()`": a relaunch into the background must
    ///    let `willRestoreState` ADOPT the session, not race it with a
    ///    fresh connect that reassigns `BLETransport.peripheral` and
    ///    implicitly cancels the very connection being adopted (§1.2).
    ///    The attempt is deferred to the first `start()` with a scene
    ///    behind it, never dropped.
    public func handleDidFinishLaunching(isForegrounded: Bool) {
        Self.log("handleDidFinishLaunching(isForegrounded: \(isForegrounded))")
        prepareForRestoration()
        setForegrounded(isForegrounded)
        Task { await start(attemptLaunchAutoConnect: isForegrounded) }
    }

    /// The same gate `autoConnectToLastKnownPeripheral()` applies, for
    /// the same reason and with the same opt-in: `xcodebuild test
    /// -only-testing:FireflyHardwareTests` launches `Firefly.app` as the
    /// test HOST, and that process must not construct a second
    /// `CBCentralManager` alongside whatever the test itself is driving
    /// over BLE. Only `FireflyApp.init()` opts in
    /// (`skipLaunchAutoConnectUnderXCTest: true`); every test that
    /// constructs `AppGraph` directly defaults to `false` and exercises
    /// the real path.
    private var shouldSkipLaunchWorkUnderXCTest: Bool {
        skipLaunchAutoConnectUnderXCTest && Self.isRunningUnderXCTest
    }

    /// M2 — the "stay connected in background" setting actually gating
    /// something (`SettingsScreen.swift`'s toggle; PR #265 review,
    /// should-fix, tracked for M2 on `stop()`'s own doc comment below).
    /// `FireflyApp.swift` calls this from its `ScenePhase` observer.
    public enum LifecyclePhase: Sendable { case foreground, background }

    public func handleScenePhaseChange(_ phase: LifecyclePhase) async {
        switch phase {
        case .background:
            // ON: do nothing — BLETransport's own reconnect-on-loss plus
            // CoreBluetooth's `bluetooth-central` background mode keep
            // the link (and this graph) alive with the screen off.
            // OFF: tear the graph down AND disconnect, right now — "off
            // = disconnect when backgrounded" (the M2 task's own words).
            guard !dependencies.store.backgroundConnectEnabled else { return }
            await stop()
        case .foreground:
            // Idempotent (`start()`'s own guard): a no-op if the graph
            // never stopped (the setting was on), and a genuine restart
            // if it did. Deliberately does NOT reconnect the client by
            // itself when the setting was off — that would silently
            // undo "off means off"; the user taps CONNECT again, same
            // as M1.
            await start()
            // A03 §3.6, the S1a review's own leftover: "the ladder …
            // evaluates `Date.now - disconnectedAt` against the table at
            // every opportunity the OS actually gives us: each
            // CoreBluetooth delegate callback, each
            // `centralManagerDidUpdateState`, and each **foreground
            // transition**". The first two are inside the transport; this
            // is the third, and it is the one that matters most after a
            // long suspension — the `Task.sleep` nudge does not run while
            // the process is suspended (§1.7), so a rung that came due at
            // 3 am is otherwise not noticed until some unrelated
            // CoreBluetooth callback happens to arrive.
            await dependencies.scanner?.appDidBecomeActive()
            // "app: automatic almanac refresh" (owner ask #1) — called
            // UNCONDITIONALLY, not folded into `start()`'s own body:
            // when `backgroundConnectEnabled` is ON (the common case),
            // `stop()` never ran on background, so `start()`'s guard
            // makes the call just above a no-op on every foreground
            // resume — exactly the resumes this feature most needs to
            // catch. `refreshIfNeeded()` carries its own 6-hour/
            // 15-minute policy, so firing it on every foreground is
            // correct: most calls simply see a fresh-enough cache and
            // do nothing.
            triggerFestpackAutoRefresh()
        }
    }

    /// PR #265 review, should-fix: M1 shipped this reachable and unused
    /// — no `ScenePhase` handling in `FireflyApp.swift`, and the Connect
    /// screen's DISCONNECT button (`ConnectScreen.swift`) called only
    /// `ConnectViewModel.disconnect()` -> `client.disconnect()`, never
    /// this method — because tearing the graph down on backgrounding was
    /// a product decision M1 had not made yet (does a backgrounded app
    /// keep tracking crew positions or not?). M2 makes it: this is now
    /// the exact thing `handleScenePhaseChange(.background)` calls when
    /// `backgroundConnectEnabled` is off — "off = disconnect when
    /// backgrounded", the M2 task's own words.
    public func stop() async {
        started = false
        core.stopObserving()
        crewMembership.stopObserving()
        privateObservation?.cancel(); privateObservation = nil
        stopObservingMyLocation()
        stopObservingIncomingTextsForNotifications()
        notificationPermissionObservation?.cancel(); notificationPermissionObservation = nil
        historyOutboxFlushObservation?.cancel(); historyOutboxFlushObservation = nil
        tickLoop?.cancel(); tickLoop = nil
        // Hardening QA pass: `stop()` used to cancel only the graph's
        // OWN subscriptions, leaving every view-model loop this graph
        // started in `makeRadarViewModel()`/`makeInboxViewModel()`/
        // `makeConnectViewModel()`/`makeLineupViewModel()` running. The
        // costly one is Radar's `recomputeLoop`: a 1 Hz `ff_radar_compute`
        // pump that nothing anywhere ever stopped, because `RadarView`
        // deliberately has no `.onDisappear` (those view models are
        // process-lifetime singletons — `makeConnectViewModel()`'s own
        // doc comment). So "off = disconnect when backgrounded" left the
        // app recomputing radar geometry once a second in the
        // background, forever, for a link it had just torn down.
        //
        // Only `radar` is reachable from here (`AppGraph` holds a weak
        // reference to it for `handlePong`; the other three view models
        // are owned by `FireflyApp` and this graph keeps no reference).
        // Radar is also the expensive one — the other three have no
        // repeating loop at all. The remaining gap is filed rather than
        // half-closed here.
        radarWasObservingAtStop = radar?.isObserving ?? false
        radar?.stopObserving()
        await uplink.stop()
        // The graph's own subscriptions stopping is not enough on its
        // own — the client (and BLETransport's own reconnect-on-loss
        // loop underneath it) would otherwise keep the radio link open
        // and reconnecting forever, exactly the thing turning this
        // setting off is supposed to prevent.
        await dependencies.client.disconnect()
    }

    /// Decode inbound portnum-269 frames and route each one to whatever
    /// actually consumes it. The DECODE is `FireflyPacket`/`ff_proto`'s
    /// (the client hands over opaque bytes on purpose); a frame
    /// `ff_proto_decode` rejects is dropped silently, the same way the
    /// client drops a malformed protobuf, rather than rendered as
    /// anything.
    // NIT (PR #275 review): no per-packet `await MainActor.run` here on
    // purpose, and this is not an oversight to "fix" later. `AppGraph`
    // is `@MainActor` (this file's own class declaration above), and a
    // `Task { ... }` created from `@MainActor`-isolated code (this
    // method) inherits that isolation for its WHOLE lifetime — not just
    // its first line. So every iteration of `for await packet in
    // stream`, and `self.handle(private: packet)` in particular, already
    // runs ON the main actor as a direct, synchronous call — there is no
    // hop to add per packet on this radio-traffic hot path, and adding
    // one would only add latency for nothing.
    private func observePrivatePackets() {
        guard privateObservation == nil else { return }
        let stream = dependencies.client.incomingPrivate()
        privateObservation = Task { [weak self] in
            for await packet in stream {
                guard let self else { return }
                self.handle(private: packet)
            }
        }
    }

    private func handle(private packet: IncomingPrivate) {
        guard let decoded = FireflyPacket.decode(packet.payload) else { return }
        switch decoded {
        case .pong(let nonce, let rssiOfUs, let snrDb):
            // The RSSI that matters for FIND is how THEY heard US — the
            // number inside the PONG body — not this packet's own rx
            // RSSI (how WE heard THEM). Two different measurements, and
            // swapping them is exactly the trap S29's "they hear us at"
            // line exists to avoid, so `packet.rssiDbm` is deliberately
            // NOT what gets passed here.
            radar?.handlePong(fromNodeID: packet.from, nonce: nonce, rssiDbm: rssiOfUs,
                               hasSNR: snrDb != nil, snrDb: Double(snrDb ?? 0))
        case .ping(let nonce):
            // Somebody is FINDing US (S29 PR 2) — a puck's own spec'd
            // behaviour, and the app must answer exactly as honestly:
            // one PONG, direct-addressed, carrying the RSSI/SNR OUR OWN
            // radio measured on THIS packet.
            replyToPing(from: packet.from, nonce: nonce, rssiDbm: packet.rssiDbm, snrDb: packet.snrDb)
        case .flare(let durationS):
            handleInboundFlare(from: packet.from, to: packet.to, durationS: durationS, packetID: packet.packetID)
        case .flareEnd:
            handleInboundFlareEnd(from: packet.from)
        case .rally(let latitude, let longitude, let name):
            handleInboundRally(from: packet.from, to: packet.to, latitude: latitude, longitude: longitude,
                                name: name, packetID: packet.packetID)
        case .rallyClear:
            handleInboundRallyClear(from: packet.from)
        case .status(let text):
            handleInboundStatus(from: packet.from, to: packet.to, text: text)
        case .ackPing, .retiredReserved01:
            // ACK_PING is reserved for v1.5 (no encoder exists yet, S04);
            // RESERVED_01 is the permanently-retired PULSE shape (S04's
            // Amendments) — both decode successfully and both are
            // honestly nothing to do, same as the puck's own
            // `app/ff_wiring.c`.
            break
        }
    }

    // MARK: - M3: id generator seeding (PR #281 review, BLOCKING 1)

    /// Raises `outbox`/`inbound`'s floor so neither can ever mint an id
    /// already sitting in `store` — the fix for the review's BLOCKING
    /// finding: `OutboxIDGenerator`/`InboundFeedIDGenerator` used to
    /// restart from the SAME fixed base on every launch, so a second
    /// session's very first send/receive could alias a first session's,
    /// letting `HistoryStore.record`'s upsert silently overwrite an
    /// unrelated persisted row, or letting a live status update
    /// (`markSent`/`setStatus(outboxID:)`) land on a restored item's row
    /// instead of the live message that actually earned it.
    ///
    /// Two sources are combined with `max`, never either alone:
    ///  1. The highest id of each id-space actually present in
    ///     `allHistory` right now (this launch's own `loadAllForRestore`
    ///     snapshot, passed in rather than re-queried).
    ///  2. `store`'s own persisted watermark (`HistoryStore.watermark
    ///     (for:)`) — durable independently of which rows currently
    ///     exist, so a row `HistoryStore.prune()` already evicted
    ///     (oldest-BY-TIMESTAMP, not oldest-by-id) cannot silently lower
    ///     the floor a past launch already proved was necessary.
    /// The combined floor is then written straight back as the new
    /// watermark (3) — even a launch that sends or receives nothing
    /// still raises the durable floor to at least what `allHistory`
    /// alone already proves, so the NEXT launch is never left relying on
    /// today's rows surviving pruning.
    ///
    /// Ids are partitioned by their OWN top bit, never by
    /// `FeedMessage.direction` — `InboundFeedIDGenerator` sets the top
    /// bit on every id it mints and `OutboxIDGenerator` never does
    /// (both types' own doc comments), and that structural partition is
    /// what actually determines which generator a given id belongs to,
    /// regardless of what this app happened to record as that message's
    /// `direction`.
    private static func seedIDGenerators(from allHistory: [(ConversationKind, FeedMessage)], store: HistoryStore,
                                          outbox: OutboxIDGenerator, inbound: InboundFeedIDGenerator) {
        let topBit: UInt64 = 0x8000_0000_0000_0000
        let maxOutboundID = allHistory.map(\.1.id).filter { $0 & topBit == 0 }.max()
        let maxInboundID = allHistory.map(\.1.id).filter { $0 & topBit != 0 }.max()

        let outboxFloor = Swift.max(maxOutboundID.map { $0 &+ 1 } ?? 1,
                                     store.watermark(for: HistoryStore.outboxWatermarkKey))
        let inboundFloor = Swift.max(maxInboundID.map { $0 &+ 1 } ?? topBit,
                                      store.watermark(for: HistoryStore.inboundWatermarkKey))

        outbox.seed(atLeast: outboxFloor)
        inbound.seed(atLeast: inboundFloor)
        store.raiseWatermark(for: HistoryStore.outboxWatermarkKey, to: outboxFloor)
        store.raiseWatermark(for: HistoryStore.inboundWatermarkKey, to: inboundFloor)
    }

    // MARK: - M3: persisted outbox flush

    /// WAITING items that survived a relaunch (`HistoryStore
    /// .pendingOutbox`) have no live `ThreadViewModel` watching them —
    /// that type's own in-memory `outbox` array only ever holds what IT
    /// personally queued THIS session (`ThreadViewModel.swift`'s own
    /// header comment). This is the flush that owns them instead: a
    /// fresh, independent `client.linkState()` subscription (S1 — this
    /// graph's own, never shared with `core`'s or any view model's), the
    /// same "flushed automatically the next time the link reaches ready"
    /// rule a live thread's own outbox follows, applied once, at the
    /// composition-root level, to whatever persisted WAITING items exist
    /// regardless of which screen — if any — is open. No double-send
    /// risk against a live `ThreadViewModel`'s own flush: a restored
    /// WAITING item was never in any `ThreadViewModel`'s session-local
    /// array to begin with (that array starts empty every launch), so
    /// the two queues can never overlap.
    private func observeHistoryOutboxFlush() {
        guard historyOutboxFlushObservation == nil else { return }
        let links = dependencies.client.linkState()
        historyOutboxFlushObservation = Task { [weak self] in
            var wasReady = false
            for await state in links {
                guard let self else { return }
                let ready = (state == .ready)
                if ready, !wasReady { await self.flushPersistedOutbox() }
                wasReady = ready
            }
        }
    }

    /// Bounded (`ThreadViewModel.outboxCap`), oldest first — the same
    /// drop-oldest FIFO discipline a live thread's own outbox follows,
    /// applied here to whatever `historyStore` still has WAITING.
    /// `item.destination` is read straight off the restored row (never
    /// re-derived): it is exactly `meshBroadcastAddress` for a
    /// whole-crew send, matching `ff_feed_item_t.to_node`'s own
    /// "0 = broadcast" convention one layer up.
    private func flushPersistedOutbox() async {
        for (_, item) in historyStore.pendingOutbox(cap: ThreadViewModel.outboxCap) {
            let dest = item.destination ?? meshBroadcastAddress
            let wantAck = (dest != meshBroadcastAddress)
            do {
                let packetID = try await dependencies.client.sendText(item.text, to: dest, wantAck: wantAck)
                inboxProvider.markSent(outboxID: item.id, packetID: packetID, at: Date())
            } catch {
                inboxProvider.setStatus(outboxID: item.id, state: .dropped, at: Date())
            }
        }
    }

    // MARK: - View models

    /// The Radar screen's view model, over the REAL bridges: `ff_crew` +
    /// `ff_radar_compute` for the view, `ff_find` for FIND, and a real
    /// portnum-269 send for its pings.
    public func makeRadarViewModel(haptics: any HapticSignaling = NoHapticSignaling()) -> RadarViewModel {
        let model = RadarViewModel(
            radar: CoreRadarComputing(crew: core.crew, radar: core.radar,
                                       linkIsReady: { [core] in core.linkState == .ready }),
            heading: dependencies.heading,
            location: dependencies.location,
            find: CoreFindSession(find: core.find, sender: packetSender),
            haptics: haptics)
        // M2: was hard-`false` here (METRIC always) — see git history on
        // this line for the reasoning that used to justify it, now moot.
        // `SettingsKey.unitsMetric`'s bool could never distinguish "the
        // user chose imperial" from "nobody has ever written this key",
        // so a naive `!store.bool(.unitsMetric)` would have made every
        // fresh install imperial by accident. `resolvedImperial()`
        // (`SettingsStoring.swift`'s "Units preference" section) is the
        // tri-state fix: `.system` — the real default — follows the
        // phone's own locale instead of guessing, and only an explicit
        // Settings-row choice ever overrides it. This is a one-time read
        // at view-model construction (every view model in this graph is
        // built once, in `FireflyApp.init` — `RootView`'s own header
        // comment), the same timing every other settings-backed field
        // here already uses; a change made mid-session on the Settings
        // screen takes effect on the next launch, not live — a narrower
        // gap than the bug this replaces, and tracked here rather than
        // silent.
        // Seeded once so the very first `compute()` (before any
        // recompute tick) is already right...
        model.imperial = dependencies.store.resolvedImperial()
        // ...and then re-read on every recompute, so a Units change made
        // mid-session on the Settings screen takes effect immediately
        // rather than on the next launch. `MapViewModel` already
        // resolved this live (`makeMapViewModel()` below); Radar did
        // not, so the two screens disagreed about units until relaunch.
        model.imperialResolver = { [dependencies] in dependencies.store.resolvedImperial() }
        radar = model
        // `makeConnectViewModel()`'s own doc comment below has the full
        // story (the NavigationSplitView detail-column remount that
        // orphans a screen-owned `.onAppear`/`.onDisappear` subscription
        // for the rest of the process). `RadarView`, `InboxContainerView`
        // and `SettingsScreen` are the same shape as `ConnectScreen` was
        // — a process-lifetime singleton shown as one of that split
        // view's `detail(for:)` destinations (`RootView.swift`) — so
        // they get the identical fix: `observe()` started HERE, once,
        // rather than left to a screen's own appear/disappear to
        // establish or tear down.
        model.observe()
        return model
    }

    /// The Inbox screen's view model, over the C-core provider and with
    /// FLARE actually available (`ThreadViewModel.flareAvailable`) for
    /// the first time — `flareSender` was `nil` in every composition
    /// until a portnum-269 send existed.
    public func makeInboxViewModel() -> InboxViewModel {
        let model = InboxViewModel(provider: inboxProvider, client: dependencies.client, flareSender: packetSender,
                                    currentFix: { [weak self] in self?.myFix },
                                    outboxIDGenerator: outboxIDGenerator, inboundFeedIDGenerator: inboundFeedIDGenerator,
                                    // A03 §3.11.3 — withdraw delivered
                                    // notifications when a thread is read.
                                    notifications: notifications)
        // Same fix as `makeRadarViewModel(haptics:)` just above, and for
        // the identical reason — see `makeConnectViewModel()`'s doc
        // comment for the full NavigationSplitView remount story this is
        // immune to by construction now. `ThreadViewModel`, which THIS
        // view model hands out per `openThread(_:)` call, is unaffected
        // and correctly stays screen-owned (`ThreadView.swift`'s own
        // `.onAppear`/`.onDisappear`) — a thread pushed via a nested
        // `NavigationStack` is genuinely per-navigation state, not a
        // `detail(for:)` destination subject to this remount at all.
        model.observe()
        return model
    }

    /// "app: festpack from fest-almanac + Lineup" — same `observe()`-once
    /// construction convention as every other screen (`makeInboxViewModel()`
    /// just above, `makeRadarViewModel(haptics:)`), so the Lineup tab's
    /// own `festpackUpdates()` subscription lives with the graph, not
    /// with whichever `detail(for:)` remount happens to show it next.
    public func makeLineupViewModel() -> LineupViewModel {
        let model = LineupViewModel(festpackProvider: festpack, picksStore: picks)
        model.observe()
        return model
    }

    public func makeConnectViewModel() -> ConnectViewModel {
        // `store:` — SHOULD-FIX 5 (PR #272 review): the "Forget this
        // node" action needs a real settings seam to clear
        // `SettingsKey.lastPeripheralID` through. `.stub()`/`.demo()`
        // each hand this an `InMemorySettingsStore()` with nothing
        // persisted (`autoConnectToLastKnownPeripheral()`'s own doc
        // comment), so FORGET is harmlessly disabled there too
        // (`canForgetNode`).
        let model = ConnectViewModel(client: dependencies.client, store: dependencies.store)
        // BUGFIX (app: fix live connect path never reaching CONNECTED on
        // macOS) — `observe()` started HERE, once, for the life of the
        // graph, exactly like `core.observe(client:...)`'s own client
        // subscriptions a few lines up in `start()`: this view model's
        // ONE `AsyncStream` from `client.linkState()` (idempotent to
        // (re-)start — `ConnectViewModel.observe()`'s own guard) is now
        // never left to `ConnectScreen`'s own `.onAppear`/`.onDisappear`
        // to establish or tear down.
        //
        // It used to be exactly that: `ConnectScreen.onAppear { connect
        // .observe() ... }` / `.onDisappear { connect.stopObserving()
        // ... }`, the same convention every other per-screen subscription
        // in this app follows (`NearbyNodesViewModel.observe()`,
        // `InboxViewModel`'s own). Link state is NOT like those — a
        // screen's own node/inbox feed is legitimately meaningless while
        // that screen is off-screen, but "is the radio connected" is
        // true or false for the WHOLE app, the moment `connect()` is
        // called from anywhere (a manual CONNECT tap, or `AppGraph`'s own
        // launch auto-connect to the remembered peripheral, well before
        // any screen has appeared at all).
        //
        // Bench-reproduced (2026-09-11, signed macOS build, launched
        // plain via `open`, no launch arguments): `NavigationSplitView`'s
        // detail column remounts ONCE at launch while the sidebar's own
        // `List(selection:)` settles its initial selection —
        // `ConnectScreen.onAppear` fires, THEN `.onDisappear` fires
        // (cancelling the subscription `stopObserving()`'s own doc
        // comment describes), and `.onAppear` never fires again even
        // though the window stays open on the Connect tab for the rest
        // of the session. `MeshtasticClient` itself connects and reaches
        // `.ready` perfectly — `BLETransport`'s own log shows the whole
        // sequence complete (`didConnect` through `central.connect
        // completed`), the want_config handshake both phases — but with
        // no subscriber left alive to hear ANY of it, `ConnectViewModel
        // .link` never leaves `.disconnected` and the Connect screen
        // reads "NOT CONNECTED" for the rest of the process, no matter
        // how many times CONNECT is tapped afterward. Owning this
        // subscription here instead — the composition root's own job per
        // this file's header comment ("subscribed when") — makes it
        // immune to that remount, or to any other screen-level lifecycle
        // churn, by construction.
        model.observe()
        return model
    }

    /// Map tab slice: crew from `core.crew` (the SAME roster Radar
    /// reads — never a second `CrewStore`), the phone's own fix/heading
    /// from `dependencies`, and a `MapFestpackSource`. The parallel
    /// festpack-foundation slice (PR #285) has now landed `FestpackProviding`
    /// on `main` — this is the "one-line swap" `MapFestpack.swift`'s own
    /// header comment anticipated: demo builds still get
    /// `DemoMapFestpackSource` (Firefly Fields must stay independent of
    /// network/real-pack availability, same call `festpack` above
    /// makes for Lineup), and every other build gets
    /// `FestpackProvidingMapAdapter` wrapping the SAME `festpack`
    /// instance `makeLineupViewModel()` reads — one provider, one
    /// composition root, never a second independent fetch. Same
    /// `dependencies.client is DemoMeshtasticClient` downcast `festpack`
    /// above and `FireflyApp.init` both already use, so there is
    /// exactly one place this decision is made, not a second flag that
    /// could drift from it.
    public func makeMapViewModel() -> MapViewModel {
        let festpackSource: any MapFestpackSource = dependencies.client is DemoMeshtasticClient
            ? DemoMapFestpackSource()
            : FestpackProvidingMapAdapter(provider: festpack)
        let model = MapViewModel(crew: core.crew, location: dependencies.location, heading: dependencies.heading,
                                  festpackSource: festpackSource, connectivity: NetworkConnectivityMonitor(),
                                  imperial: { [dependencies] in dependencies.store.resolvedImperial() })
        // PR #283 review, BLOCKING 3: deliberately NOT `model.observe()`
        // here, unlike `makeRadarViewModel`/`makeInboxViewModel`/
        // `makeConnectViewModel` just above. Those three are each
        // genuinely process-lifetime state (`makeConnectViewModel()`'s
        // own doc comment draws the exact line: link state is true/false
        // for the WHOLE app, not meaningful only while one screen is on
        // top). `MapViewModel`'s 1 Hz `pinRefreshLoop` plus its
        // location/heading/connectivity subscriptions exist ONLY to
        // refresh the Map tab's OWN UI (pin age text, the festpack
        // projection, the offline chip) — no other screen consumes any
        // of it — so this is the "genuinely screen-scoped" category
        // `NearbyNodesViewModel`/`ThreadViewModel`/`DiagnosticsViewModel`
        // already follow: `MapTabView`'s own `.onAppear { model.observe()
        // }` / `.onDisappear { model.stopObserving() }` (Map/MapTabView.swift)
        // starts and stops it, so the loop — and the battery it costs —
        // stops the moment the Map tab is no longer visible, not just at
        // app exit.
        return model
    }
}
