//
//  RootView.swift — the app's navigation skeleton (Slice C owns this
//  file and its destination registry; slices D and E each change
//  exactly one line in the `detail(for:)` switch below — see docs/specs/
//  A01-companion-app.md, "Slices", the shared-file table).
//
//  Four tabs on the bar (docs/specs/A01-companion-app.md, "Navigation"
//  — owner decision, 2026-09-13): Find, Inbox, Lineup, More. Find
//  replaces the old separate Radar and Map tabs with ONE tab and a
//  segmented control (Radar · Map · Field, Radar the default) — one
//  tab-bar question ("where do I look for my crew?") instead of two,
//  and bigger tap targets for gloved hands at a festival. Connect and
//  Settings, which used to be their own tabs, are `MoreScreen` rows —
//  see that file's own header comment for why they PUSH the existing
//  screens rather than re-implementing them.
//
import FireflyMesh
import FireflyModel
import SwiftUI

enum Destination: String, CaseIterable, Identifiable {
    // "app: Find tab — Radar · Map · Field segments, four-tab bar
    // (owner decision)" — replaces the old separate `.radar`/`.map`
    // cases; see `FindScreen.swift`.
    case find = "Find"
    case inbox = "Inbox"
    // "app: festpack from fest-almanac + Lineup".
    case lineup = "Lineup"
    // "app: five-tab bar per design" — replaces the old `.connect`/
    // `.settings` cases; see `MoreScreen.swift`.
    case more = "More"

    var id: String { rawValue }

    var systemImage: String {
        switch self {
        case .find: return "location.north.line"
        case .inbox: return "tray"
        case .lineup: return "music.mic"
        case .more: return "ellipsis"
        }
    }
}

struct RootView: View {
    let connect: ConnectViewModel
    let settings: SettingsViewModel
    let channelImport: ChannelImportViewModel
    let client: any MeshtasticClientProtocol
    // Slice E's hunk (A01's Slices table: "Slices C, D, E each change
    // exactly one of those [switch] lines" — constructing that line's
    // view needs this view model reference alongside `connect`'s).
    let inbox: InboxViewModel
    // Every view model is constructed ONCE, in `FireflyApp.init`, from
    // the one `AppGraph` — never inside a destination's own `init`,
    // which would build a second client the moment the destination was
    // shown (`AppGraph`'s header comment).
    let radar: RadarViewModel
    /// "app: festpack from fest-almanac + Lineup" — same
    /// process-lifetime-singleton shape as `radar`/`inbox` just above
    /// (`AppGraph.makeLineupViewModel()`'s own doc comment).
    let lineup: LineupViewModel
    /// The BLE node picker's scan seam — nil wherever there is no radio
    /// (the stub stack, the iOS Simulator), which the Connect screen
    /// renders as an honestly empty picker.
    let scanner: (any NodeScanning)?
    /// Non-nil in exactly one case: `FireflyApp.init` built a demo
    /// graph (`-FireflyDemo`/`FIREFLY_DEMO=1`, simulator-only). This is
    /// ALSO the one source of truth for whether the DEMO badge shows —
    /// never a second flag that could drift from it.
    var demoRunner: DemoRunner?
    /// `-FireflyDemoScreen <name>`'s parsed value — see
    /// `DemoLaunch.requestedScreen`. `nil` outside demo mode.
    var initialDemoScreen: String?
    /// M2: inbound FLARE's full-screen takeover state
    /// (`docs/specs/S10-flare.md`). Owned by `AppGraph` like every other
    /// `ff_*`-backed view model — one per process, same as `radar`/`inbox`.
    var flareTakeover: FlareTakeoverViewModel
    /// M2's hunk: the one place a crew member is paired/unpaired/
    /// renamed (`AppGraph.crewPairing`'s own doc comment) — handed to
    /// Connect (Nearby's Add/Remove) and Settings (the Crew section).
    let pairing: CrewPairingController
    /// Map tab slice's hunk: `AppGraph.makeMapViewModel()`'s one view
    /// model, same "built once by the graph, never by a screen's own
    /// init" rule every other destination here follows.
    let map: MapViewModel
    /// Map tab slice: the selected-crew card's FIND action. PR #283
    /// review, BLOCKING 2: this closure (`FireflyApp.swift`) calls the
    /// SAME `RadarViewModel.startFind(targetNodeID:)` Radar's own FIND
    /// button calls — never the raw `ff_find` bridge directly — so the
    /// session this starts actually ticks (sends pings) rather than
    /// sitting "active" and silent until the user manually restarts it
    /// from Radar. This view switches to Radar afterward (where FIND's
    /// own UI already lives, `RadarView.swift`) rather than duplicating
    /// a FIND affordance inside Map.
    let mapFind: (UInt32) -> Void
    /// Map tab slice: the selected-crew card's MESSAGE action switches
    /// to Inbox. KNOWN GAP, flagged rather than silently faked: this
    /// does not yet deep-link to that member's own thread — doing so
    /// needs `InboxContainerView`'s demo-only `demoInitialThread` seam
    /// generalized to a live "open this thread" request, which is a
    /// separate, larger change than this slice's own scope. Tracked
    /// here, not hidden.
    let mapMessage: (UInt32) -> Void
    /// "app: five-tab bar per design" — true when a radio is already
    /// bonded (`SettingsKey.bondedPeripheralIDs`/`.lastPeripheralID`,
    /// `AppDependencies.live()`) or a `-FireflyAutoConnect <name>` debug
    /// launch is in flight (`FireflyAutoConnectLaunch`) — i.e. this
    /// process already knows which radio it is going after. `false`
    /// on a fresh install/simulator run with nothing paired yet.
    /// Decides the ONE thing this file otherwise used to hardcode: does
    /// launch land on Radar (there is somewhere useful to look already)
    /// or on Connect (there is nothing to show until one is picked)?
    /// See this type's own `.task` below and the PR body for why this
    /// reads persisted state once at construction rather than the
    /// live, still-connecting `ConnectViewModel` (which has nothing
    /// meaningful to report yet at the exact moment this view's first
    /// frame renders).
    let hasKnownRadio: Bool
    @State private var selection: Destination = .more
    /// Which row `MoreScreen` should push into the moment it next
    /// processes it (or, on macOS, the moment a sidebar row changes this
    /// while More is already selected) — `nil` opens on the plain list.
    /// Set once at launch (below) for "no known radio yet" and for
    /// `-FireflyDemoScreen connect`/`settings`/`diagnostics`; set again
    /// on every macOS sidebar tap on one of More's own rows. ONE-SHOT:
    /// `MoreScreen` calls `onAutoOpenHandled` (below, `detail(for:)`'s
    /// `.more` case) the instant it acts on a value here, which clears
    /// it straight back to `nil` — see `MoreScreen`'s own header
    /// comment for the "tapping More re-pushes Connect" bug a sticky
    /// version of this flag used to cause.
    @State private var moreAutoOpen: MoreScreen.Row?
    /// `MoreScreen`'s own push stack, lifted up here (rather than kept
    /// as that screen's private `@State`) so a genuine tab-reselect —
    /// detected by `selectionBinding` below — can reset it to `[]` from
    /// OUTSIDE that view: the standard iOS "tap the active tab pops its
    /// nav stack to root" behaviour, implemented explicitly rather than
    /// relied on, since this app's own `autoOpen` re-push bug (see
    /// `MoreScreen`'s header comment) is exactly what happens when that
    /// system gesture's result gets silently undone a moment later.
    @State private var morePath: [MoreScreen.Row] = []
    /// "app: Find tab — Radar · Map · Field segments" — which segment
    /// `FindScreen` shows, Radar the default (owner decision). Hoisted
    /// up here rather than kept as `FindScreen`'s own `@State`, same
    /// reason `morePath` is (that property's own doc comment): on
    /// macOS, `detail(for:)`'s `@ViewBuilder switch` constructs a FRESH
    /// `FindScreen` every time `selection` moves away from `.find` and
    /// back, which would otherwise reset the segment on every trip
    /// through another sidebar row — "remember the selected segment
    /// across tab switches within a session" (owner decision) needs
    /// state that survives that reconstruction. On iOS, where `TabView`
    /// keeps every tab's own view alive, this would have worked as
    /// local `@State` too, but the two platforms sharing one `RootView`
    /// means the state has to live wherever the platform that CAN'T get
    /// away with local `@State` needs it.
    @State private var findSegment: FindSegment = .radar

    /// iOS's `TabView(selection:)` binding, wrapping `$selection` so a
    /// tap on the ALREADY-selected More tab — which changes nothing
    /// about `selection` itself — is still observable here: pop
    /// `morePath` back to `[]` in that one case, exactly the "re-tap the
    /// active tab" behaviour standard iOS tab bars give a plain
    /// `NavigationStack`-per-tab for free, made explicit here so it does
    /// not depend on that system gesture actually firing (see
    /// `MoreScreen`'s header comment on why relying on it silently
    /// wasn't enough). Every other tab keeps ordinary `$selection`
    /// semantics — reselecting Find/Inbox/Lineup does nothing beyond
    /// what always happened. In particular, leaving Find for another
    /// tab and coming back does NOT reset `findSegment` — that
    /// property is not tied to `selection` at all, which is the whole
    /// point of hoisting it up here (its own doc comment).
    private var selectionBinding: Binding<Destination> {
        Binding(
            get: { selection },
            set: { newValue in
                if newValue == .more, selection == .more {
                    morePath = []
                }
                selection = newValue
            }
        )
    }

    var body: some View {
        // A ZStack, not a plain VStack, so the FLARE takeover (below) can
        // sit ON TOP of everything else, including the DEMO strip — S10:
        // "full-screen takeover... regardless of current face." Unlike
        // the DEMO badge (a thin strip that never covers content, on
        // purpose), a FLARE takeover is DELIBERATELY the one thing in
        // this app allowed to cover the whole screen: that is the entire
        // point of "come find me" urgency. `.fullScreenCover` is
        // iOS-only (no macOS equivalent), so this plain top layer is
        // this app's own cross-platform substitute.
        ZStack {
            // A full-width strip stacked ABOVE `content`, not an overlay
            // on top of it (see `DemoBadge`'s own header comment):
            // reserving real layout space here pushes every screen's own
            // nav bar down instead of racing it for the same row, so the
            // badge can never collide with a title again, on any screen.
            VStack(spacing: 0) {
                if demoRunner != nil {
                    DemoBadge()
                }
                content
            }
            .task { applyInitialSelection() }
            .task { await runInitialDemoScreen() }
            // The Find tab's start/stop rule is driven from HERE, off
            // this view's own `selection`/`findSegment` state, rather
            // than from `FindScreen`'s `onAppear`/`onDisappear` — see
            // `applyFindLifecycle()`'s own comment for the measured
            // launch trace that requires it.
            .onChange(of: selection, initial: true) { _, _ in applyFindLifecycle() }
            .onChange(of: findSegment) { _, _ in applyFindLifecycle() }

            if flareTakeover.isActive {
                FlareTakeoverView(model: flareTakeover)
                    .transition(.opacity)
                    .zIndex(1)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: flareTakeover.isActive)
    }

    @ViewBuilder
    private var content: some View {
        #if os(macOS)
        NavigationSplitView {
            List(selection: $selection) {
                ForEach([Destination.find, .inbox, .lineup]) { destination in
                    Label(destination.rawValue, systemImage: destination.systemImage)
                        .tag(destination)
                }
                // "app: five-tab bar per design" — grouped the same way
                // the iOS tab bar groups them (Find/Inbox/Lineup direct,
                // then a More section), just laid out flat rather than
                // behind a tap: a Mac sidebar has the room,
                // so Connect/Settings/System show as their own rows
                // instead of needing `MoreScreen`'s own list opened
                // first. Plain buttons, not `.tag()`-bound selection
                // rows like the four above: each one has to set BOTH
                // `selection` (so the detail column actually shows
                // `MoreScreen`) and `moreAutoOpen` (so it knows which of
                // its rows to push into), which `List(selection:)`'s
                // own single-value binding cannot express.
                Section("More") {
                    moreSidebarRow(.connect, title: "Connect", systemImage: "antenna.radiowaves.left.and.right")
                    moreSidebarRow(.settings, title: "Settings", systemImage: "slider.horizontal.3")
                    moreSidebarRow(.system, title: "System", systemImage: "waveform.path.ecg")
                }
            }
            .navigationSplitViewColumnWidth(min: 160, ideal: 180)
        } detail: {
            // `path: $morePath` here for the SAME reason the iOS `.more`
            // tab's own `NavigationStack` gets it (see `morePath`'s doc
            // comment): `MoreScreen`'s `.navigationDestination(for:
            // Row.self)` needs a real, externally-readable/-writable
            // path to push into, not this container's own private,
            // opaque one. Harmless for every OTHER selection — nothing
            // but `MoreScreen` ever pushes a `Row` value onto it, so it
            // just stays empty while Find/Inbox/Lineup are showing.
            NavigationStack(path: $morePath) {
                detail(for: selection)
            }
        }
        #else
        TabView(selection: selectionBinding) {
            ForEach(Destination.allCases) { destination in
                Group {
                    if destination == .more {
                        // The only tab with an explicit, externally
                        // resettable path — see `morePath`'s own doc
                        // comment on why More alone needs this.
                        NavigationStack(path: $morePath) {
                            detail(for: destination)
                        }
                    } else {
                        NavigationStack {
                            detail(for: destination)
                        }
                    }
                }
                .tabItem { Label(destination.rawValue, systemImage: destination.systemImage) }
                .tag(destination)
            }
        }
        .tint(.ffAmber)
        #endif
    }

    #if os(macOS)
    private func moreSidebarRow(_ row: MoreScreen.Row, title: String, systemImage: String) -> some View {
        Button {
            selection = .more
            moreAutoOpen = row
        } label: {
            Label(title, systemImage: systemImage)
        }
        .buttonStyle(.plain)
    }
    #endif

    @ViewBuilder
    private func detail(for destination: Destination) -> some View {
        switch destination {
        case .find: FindScreen(radar: radar, map: map, segment: $findSegment,
                                colorblind: settings.colorblindPalette,
                                // FIND (from a Map pin card) switches THIS tab's own
                                // segment to Radar — the FIND affordance's own UI lives
                                // there (`RadarView.swift`) — never the outer tab, since
                                // we are already on it. MESSAGE switches the OUTER tab
                                // to Inbox, which only `RootView` can do.
                                onFind: { nodeID in mapFind(nodeID); findSegment = .radar },
                                onMessage: { nodeID in mapMessage(nodeID); selection = .inbox })
        case .inbox: InboxContainerView(model: inbox, demoInitialThread: demoThreadTarget,
                                         colorblind: settings.colorblindPalette,
                                         // Owner note (build 304, item 3): Inbox's own
                                         // "no crew paired" empty state used to just SAY
                                         // "pair crew from the Crew screen" with nothing
                                         // to tap — this switches to More and lands on
                                         // Connect's Nearby section, the one place
                                         // pairing actually happens.
                                         onPairCrew: { selection = .more; moreAutoOpen = .connect })
        case .lineup: LineupScreen(model: lineup)
        case .more: MoreScreen(connect: connect, settings: settings, channelImport: channelImport,
                                client: client, lineup: lineup, scanner: scanner, pairing: pairing,
                                colorblind: settings.colorblindPalette,
                                autoOpenDiagnosticsInSettings: initialDemoScreen == "diagnostics",
                                autoOpen: moreAutoOpen,
                                onAutoOpenHandled: { moreAutoOpen = nil },
                                path: $morePath)
        }
    }

    private var demoThreadTarget: ConversationKind? {
        switch initialDemoScreen {
        case "thread": return .member(DemoCrew.taylor)
        // M2: the RALLY screenshot needs the CREW thread open, since
        // that is where a broadcast RALLY (S04's default addressing)
        // lands.
        case "rally": return .crew
        default: return nil
        }
    }

    /// "app: Find tab — Radar · Map · Field segments" — which segment
    /// `-FireflyDemoScreen <name>` should land Find on. `nil` for every
    /// launch argument that is not one of these (Radar is `findSegment`'s
    /// own default, so "radar"/"radar-signal"/"find" need no override
    /// here — see `runInitialDemoScreen()`, which reads this).
    /// "map-gps"/"map-field" are the ORIGINAL names (kept for whatever
    /// already scripts against them); "map"/"field" are the bare names
    /// the owner's brief itself uses.
    private var initialFindSegment: FindSegment? {
        switch initialDemoScreen {
        case "map", "map-gps": return .map
        case "field", "map-field": return .field
        default: return nil
        }
    }

    /// "Only the visible segment's view model observes/pumps" (owner
    /// decision, 2026-09-13) — applied from `selection`/`findSegment`,
    /// the two pieces of state that actually define which segment is on
    /// screen, on a view that is never remounted.
    ///
    /// MEASURED, not reasoned (review of this PR). Wiring this to
    /// `FindScreen`'s own `onAppear`/`onDisappear` — the obvious place
    /// — reproduces the `NavigationSplitView` detail-column remount
    /// `ConnectScreen.swift`'s own `.onAppear` comment documents.
    /// Instrumented macOS launch, Find as the detail destination:
    ///
    ///     onAppear segment=radar
    ///     onAppear segment=radar
    ///     onDisappear
    ///
    /// — two mounts, and the FIRST instance's `onDisappear` arriving
    /// after the second's `onAppear`. `stopAll()` therefore ran last
    /// and left BOTH `RadarViewModel` and `MapViewModel` stopped while
    /// Find was on screen: a frozen Radar on the app's own landing
    /// destination, for the rest of the process or until the user
    /// happened to tap a segment. `selection`/`findSegment` are plain
    /// `@State` on this view, which that remount does not touch, so the
    /// same rule applied from here cannot be orphaned by it.
    private func applyFindLifecycle() {
        if selection == .find {
            FindLifecycle.apply(segment: findSegment, radar: radar, map: map)
        } else {
            FindLifecycle.stopAll(radar: radar, map: map)
        }
    }

    /// "app: five-tab bar per design" — the one place `hasKnownRadio`
    /// is actually consulted: a radio already known -> land on Radar,
    /// otherwise land on More with Connect pre-pushed (`MoreScreen`'s
    /// own `autoOpen`), so a fresh install still reaches Connect with
    /// zero taps, exactly as a plain launch always has. Runs in its own
    /// `.task`, separate from `runInitialDemoScreen()`: that one awaits
    /// `demoRunner.waitUntilStarted()` before touching `selection` at
    /// all, so on any launch that also passes `-FireflyDemoScreen`,
    /// THIS synchronous assignment always lands first and the demo
    /// screen's own choice (once it resolves) always wins — never a
    /// race between the two.
    private func applyInitialSelection() {
        if hasKnownRadio {
            selection = .find
        } else {
            selection = .more
            moreAutoOpen = .connect
        }
        // "app: Map subscribes to festpack updates" (2026-09-13) —
        // `-FireflyStartTab`/`-FireflyFindSegment`, debug-only
        // (`FireflyDebugStartDestinationLaunch`'s own header comment):
        // lands on a specific tab/segment against the REAL composition
        // graph, for proving the Field forever-spinner fix against a
        // live `AlmanacFestpackProvider` on a fresh simulator install
        // where `hasKnownRadio` is honestly `false`. `nil` (and this
        // whole block a no-op) outside `DEBUG` and on every ordinary
        // launch that does not pass either argument. The name -> enum
        // match happens HERE, not in `FireflyDebugStartDestinationLaunch`
        // itself — see that file's own header comment for why it hands
        // back a bare `String?` rather than `Destination`/`FindSegment`
        // directly.
        if let tabName = FireflyDebugStartDestinationLaunch.requestedTabName(),
           let tab = Destination.allCases.first(where: { $0.rawValue.caseInsensitiveCompare(tabName) == .orderedSame }) {
            selection = tab
        }
        if let segmentName = FireflyDebugStartDestinationLaunch.requestedFindSegmentName(),
           let segment = FindSegment.allCases.first(where: { $0.rawValue.caseInsensitiveCompare(segmentName) == .orderedSame }) {
            findSegment = segment
        }
    }

    /// Polls (rather than sleeping a fixed budget) until the Lineup
    /// view model has a pack to seed picks/a detail sheet from — the
    /// same "no fixed sleeps" rule the test suite's own `eventually`
    /// helper follows, applied here because a fixed 300 ms silently
    /// produced an EMPTY screenshot whenever the bundled pack happened
    /// to parse slower than that. Bounded so a genuinely broken load
    /// still falls through to the screen's own honest empty state.
    private func waitForLineupFestpack(timeout: TimeInterval = 5) async {
        let deadline = Date().addingTimeInterval(timeout)
        while lineup.festpack == nil, Date() < deadline {
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
    }

    /// Maps `-FireflyDemoScreen <name>` to a tab selection plus, for
    /// the handful of names that need more than a tab (a no-GPS fix, a
    /// running FIND session), the one extra `DemoRunner` call that gets
    /// it there. `InboxContainerView`/`SettingsScreen` handle "thread"/
    /// "diagnostics" themselves (see their own `.task`s) — this only
    /// owns tab selection and the two Radar variants.
    private func runInitialDemoScreen() async {
        guard let demoRunner, let initialDemoScreen else { return }
        // `FireflyApp`'s `await graph.start(); await demoRunner?.start()`
        // runs in a SEPARATE `.task` from this one — no ordering
        // guarantee between them otherwise — so anything that touches
        // `demoRunner` beyond a bare tab selection waits for `start()`
        // to actually finish seeding the world first (`DemoRunner
        // .isStarted`'s own doc comment: calling `withdrawPhoneFix()`
        // before `start()`'s `location.setFix(world.phoneFix)` has run
        // would have the LATER call silently put the fix right back).
        await demoRunner.waitUntilStarted()
        switch initialDemoScreen {
        case "connect":
            selection = .more
            moreAutoOpen = .connect
        case "radar":
            selection = .find
            findSegment = .radar
        case "radar-signal":
            selection = .find
            findSegment = .radar
            demoRunner.withdrawPhoneFix()
        case "find":
            // "app: Find tab — Radar · Map · Field segments" — this
            // name predates the tab itself (it started a FIND ping
            // session on Taylor, which lives on the Radar segment); now
            // it also has to pick the right SEGMENT, not just the right
            // tab.
            selection = .find
            findSegment = .radar
            try? await Task.sleep(nanoseconds: 300_000_000)
            demoRunner.startFindOnTaylor()
        case "inbox", "thread":
            selection = .inbox
        case "flare":
            // The takeover renders on top of whatever tab is selected
            // (S10: "regardless of current face") — Radar just gives the
            // screenshot a sensible screen underneath it.
            selection = .find
            findSegment = .radar
            try? await Task.sleep(nanoseconds: 300_000_000)
            demoRunner.triggerInboundFlare()
        case "rally":
            // Inject FIRST and wait for it to actually land in the feed
            // (the inbound path is a real `EventHub` -> `AppGraph
            // .handle(private:)` round trip, not synchronous) — THEN
            // select Inbox, so `InboxContainerView`'s own demo-thread
            // `.task` opens the CREW thread onto a feed that already has
            // the RALLY in it. Reversing this order would race
            // `ThreadViewModel.observe()`'s one-shot `refresh()` against
            // the still-in-flight push.
            demoRunner.triggerInboundRally()
            try? await Task.sleep(nanoseconds: 400_000_000)
            selection = .inbox
        case "lineup":
            selection = .lineup
        case "lineup-picks":
            // "app: Lineup by-stage grid, day pills, My picks" — a
            // screenshot-only seam, same shape as "map-gps"/"map-field"
            // just below: pre-seed a couple of picks on the day the
            // grid itself opens on (rather than trying to synthesize a
            // tap on a specific grid block from a launch argument) so
            // the My picks screenshot shows real conflict-marker/star
            // content instead of the empty state.
            selection = .lineup
            lineup.selectedTab = .picks
            await waitForLineupFestpack()
            if let festpack = lineup.festpack, let night = lineup.selectedNightDayOfYear {
                for set in festpack.sets.filter({ $0.nightDayOfYear == night && $0.startMinute != nil }).prefix(2) {
                    lineup.togglePick(set)
                }
            }
        case "lineup-detail":
            // Same idea as "lineup-picks": opens the Grid with the
            // first known-start set's detail sheet already showing, so
            // a screenshot script never has to simulate a tap on a
            // specific block's screen position.
            selection = .lineup
            await waitForLineupFestpack()
            if let festpack = lineup.festpack, let night = lineup.selectedNightDayOfYear,
               let set = festpack.sets.first(where: { $0.nightDayOfYear == night && $0.startMinute != nil }) {
                lineup.selectSet(set)
            }
        case "settings", "diagnostics":
            selection = .more
            moreAutoOpen = .settings
        case "map", "map-gps", "field", "map-field":
            // "app: Find tab — Radar · Map · Field segments" — these
            // land on Find, on the segment `initialFindSegment` picks
            // for this exact name.
            selection = .find
            findSegment = initialFindSegment ?? .map
        default:
            break
        }
    }
}

struct PlaceholderView: View {
    let title: String
    let note: String

    var body: some View {
        VStack(spacing: 16) {
            Text(title.uppercased())
                .font(.system(.title2, design: .rounded).weight(.bold))
                .foregroundStyle(Color.ffInk)
            Text(note)
                .font(.callout)
                .foregroundStyle(Color.ffCaption)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
            Text("NOT BUILT YET")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(Color.ffAmber)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
    }
}
