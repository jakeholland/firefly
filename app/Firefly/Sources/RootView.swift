//
//  RootView.swift — the app's navigation skeleton (Slice C owns this
//  file and its destination registry; slices D and E each change
//  exactly one line in the `detail(for:)` switch below — see docs/specs/
//  A01-companion-app.md, "Slices", the shared-file table).
//
//  Five tabs on the bar (docs/specs/A01-companion-app.md, "Navigation"):
//  Radar, Map, Inbox, Lineup, More — matching the approved design
//  (RadarSignal.dc.html/MapLive.dc.html mocks' own tab bar) and staying
//  under iOS's five-tab cap before `UITabBarController` starts
//  auto-generating its own unstyled "More" overflow list. Connect and
//  Settings, which used to be their own tabs, are now `MoreScreen` rows
//  — see that file's own header comment for why they PUSH the existing
//  screens rather than re-implementing them.
//
import FireflyMesh
import FireflyModel
import SwiftUI

enum Destination: String, CaseIterable, Identifiable {
    case radar = "Radar"
    // Map tab slice: appended, not inserted (this file's own
    // "each slice changes exactly one line" convention).
    case map = "Map"
    case inbox = "Inbox"
    // "app: festpack from fest-almanac + Lineup".
    case lineup = "Lineup"
    // "app: five-tab bar per design" — replaces the old `.connect`/
    // `.settings` cases; see `MoreScreen.swift`.
    case more = "More"

    var id: String { rawValue }

    var systemImage: String {
        switch self {
        case .radar: return "location.north.line"
        case .map: return "map"
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
    /// appears (or, on macOS, the moment a sidebar row changes this
    /// while More is already selected) — `nil` opens on the plain list.
    /// Set once at launch (below) for "no known radio yet" and for
    /// `-FireflyDemoScreen connect`/`settings`/`diagnostics`; set again
    /// on every macOS sidebar tap on one of More's own rows.
    @State private var moreAutoOpen: MoreScreen.Row?

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
                ForEach([Destination.radar, .map, .inbox, .lineup]) { destination in
                    Label(destination.rawValue, systemImage: destination.systemImage)
                        .tag(destination)
                }
                // "app: five-tab bar per design" — grouped the same way
                // the iOS tab bar groups them (Radar/Map/Inbox/Lineup
                // direct, then a More section), just laid out flat
                // rather than behind a tap: a Mac sidebar has the room,
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
            NavigationStack {
                detail(for: selection)
            }
        }
        #else
        TabView(selection: $selection) {
            ForEach(Destination.allCases) { destination in
                NavigationStack {
                    detail(for: destination)
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
        case .radar: RadarView(model: radar, colorblind: settings.colorblindPalette)
        case .map: MapTabView(model: map, initialSegment: initialMapSegment ?? .field,
                               colorblind: settings.colorblindPalette,
                               onFind: { nodeID in mapFind(nodeID); selection = .radar },
                               onMessage: { nodeID in mapMessage(nodeID); selection = .inbox })
        case .inbox: InboxContainerView(model: inbox, demoInitialThread: demoThreadTarget,
                                         colorblind: settings.colorblindPalette)
        case .lineup: LineupScreen(model: lineup)
        case .more: MoreScreen(connect: connect, settings: settings, channelImport: channelImport,
                                client: client, lineup: lineup, scanner: scanner, pairing: pairing,
                                colorblind: settings.colorblindPalette,
                                autoOpenDiagnosticsInSettings: initialDemoScreen == "diagnostics",
                                autoOpen: moreAutoOpen)
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

    /// Map tab slice: `-FireflyDemoScreen map-gps`/`map-field` pick
    /// which segment `MapTabView` opens on — `nil` (its own default,
    /// `.field`) for every other launch argument.
    private var initialMapSegment: MapSegment? {
        switch initialDemoScreen {
        case "map-gps": return .gps
        case "map-field": return .field
        default: return nil
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
            selection = .radar
        } else {
            selection = .more
            moreAutoOpen = .connect
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
            selection = .radar
        case "radar-signal":
            selection = .radar
            demoRunner.withdrawPhoneFix()
        case "find":
            selection = .radar
            try? await Task.sleep(nanoseconds: 300_000_000)
            demoRunner.startFindOnTaylor()
        case "inbox", "thread":
            selection = .inbox
        case "flare":
            // The takeover renders on top of whatever tab is selected
            // (S10: "regardless of current face") — Radar just gives the
            // screenshot a sensible screen underneath it.
            selection = .radar
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
        case "settings", "diagnostics":
            selection = .more
            moreAutoOpen = .settings
        case "map-gps", "map-field":
            selection = .map
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
                .foregroundStyle(Color.ffMuted)
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
