//
//  RootView.swift — the milestone-1 navigation skeleton (Slice C owns
//  this file and its destination registry; slices D and E each change
//  exactly one line in the `switch` below — see docs/specs/
//  A01-companion-app.md, "Slices", the shared-file table).
//
//  Four destinations, matching the screens A01 scopes for M1: Connect,
//  Radar, Inbox, Settings. Radar and Inbox stay placeholders here —
//  they state what they will show and, deliberately, show NOTHING
//  ELSE; a screen that invents data is exactly the failure this whole
//  product is designed against (docs/ARCHITECTURE.md, "Honest state").
//
import FireflyMesh
import FireflyModel
import SwiftUI

enum Destination: String, CaseIterable, Identifiable {
    case connect = "Connect"
    case radar = "Radar"
    case inbox = "Inbox"
    // "app: festpack from fest-almanac + Lineup" — appended per this
    // file's own convention (each slice changes exactly one `switch`
    // line, never reorders another's).
    case lineup = "Lineup"
    // Map tab slice: appended, not inserted, so Connect/Radar/Inbox/
    // Settings' own tags/order never move (an append-only hunk, per
    // this file's own header comment on how slices C/D/E each touch
    // this switch).
    case map = "Map"
    case settings = "Settings"

    var id: String { rawValue }

    var systemImage: String {
        switch self {
        case .connect: return "antenna.radiowaves.left.and.right"
        case .radar: return "location.north.line"
        case .inbox: return "tray"
        case .lineup: return "music.mic"
        case .map: return "map"
        case .settings: return "slider.horizontal.3"
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
    @State private var selection: Destination = .connect

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
            List(Destination.allCases, selection: $selection) { destination in
                Label(destination.rawValue, systemImage: destination.systemImage)
                    .tag(destination)
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

    @ViewBuilder
    private func detail(for destination: Destination) -> some View {
        switch destination {
        case .connect: ConnectScreen(connect: connect, client: client, channelImport: channelImport,
                                      scanner: scanner, pairing: pairing, colorblind: settings.colorblindPalette)
        case .radar: RadarView(model: radar, colorblind: settings.colorblindPalette)
        case .inbox: InboxContainerView(model: inbox, demoInitialThread: demoThreadTarget,
                                         colorblind: settings.colorblindPalette)
        case .lineup: LineupScreen(model: lineup)
        case .map: MapTabView(model: map, initialSegment: initialMapSegment ?? .field,
                               colorblind: settings.colorblindPalette,
                               onFind: { nodeID in mapFind(nodeID); selection = .radar },
                               onMessage: { nodeID in mapMessage(nodeID); selection = .inbox })
        case .settings: SettingsScreen(model: settings, client: client, pairing: pairing, lineup: lineup,
                                        autoOpenDiagnostics: initialDemoScreen == "diagnostics")
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
            selection = .connect
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
            selection = .settings
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
