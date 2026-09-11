//
//  ConnectViewModel.swift — the Connect screen's state.
//
//  The MVVM shape every other view model in this app follows:
//    - `@Observable`, `@MainActor`, no Combine;
//    - it holds a PROTOCOL (`MeshtasticClientProtocol`), never a
//      concrete client, so the same view model drives a real Heltec over
//      BLE on a Mac and a stub in a unit test or the iOS Simulator;
//    - it owns no I/O of its own: it consumes the client's
//      `AsyncStream`s and publishes plain values.
//
import FireflyMesh
import Foundation
import Observation

@MainActor
@Observable
public final class ConnectViewModel {
    public private(set) var link: LinkState = .disconnected
    /// Non-nil only after a real failure, and it says what failed.
    public private(set) var lastError: String?
    /// M2 — "link-state UI showing... 'last connected X ago'". The
    /// moment `.ready` was last observed; nil until the first one ever
    /// arrives. Never cleared by a later non-ready state — that is
    /// exactly what makes "X ago" meaningful while reconnecting or
    /// disconnected.
    public private(set) var lastConnectedAt: Date?

    /// Owner feedback from the first real-radio run: "iPhone shows
    /// 'connected' but I'm not sure to which radio" — this is the fix.
    /// Everything the Connect screen needs to answer "which radio am I
    /// on?" without re-deriving it itself (MVVM convention 5, docs/specs/
    /// A01-companion-app.md: "converts state into display strings in
    /// the view model, not in the view"). `bleName` comes from the
    /// picker at CONNECT-tap time (`noteSelectedPeripheral(name:rssiDbm:)`
    /// — this view model has no BLE stack of its own to learn one from
    /// any other way); `longName` comes off the connected node's own
    /// `NodeInfo`, once want_config's nodeDB phase reports it
    /// (`apply(_:MeshNodeSnapshot)`, below). Every field stays nil
    /// rather than guessed — the same "never invents" rule every other
    /// honestly-optional field in this app follows.
    public struct ConnectedRadioSummary: Equatable, Sendable {
        public var bleName: String?
        public var longName: String?
        /// The picker's scan-time RSSI for the peripheral CONNECT was
        /// tapped on — there is no live post-connect BLE RSSI poll in
        /// this app (`BLETransport` never reads one), so this is
        /// honestly "last known before the radio stopped advertising",
        /// not a live meter.
        public var rssiDbm: Int?

        public init(bleName: String? = nil, longName: String? = nil, rssiDbm: Int? = nil) {
            self.bleName = bleName
            self.longName = longName
            self.rssiDbm = rssiDbm
        }
    }
    public private(set) var connectedRadio: ConnectedRadioSummary?

    private let client: any MeshtasticClientProtocol
    /// SHOULD-FIX 5 (PR #272 review) — "Forget this node". Optional, and
    /// appended after `client` with a `nil` default, so every existing
    /// `ConnectViewModel(client:)` call site (every test in this file,
    /// `CoreStoreTests`, `DemoRunnerTests`) keeps compiling unchanged.
    /// `nil` in exactly those tests and in any other composition that
    /// has no settings seam at all — `forgetNode()`/`canForgetNode`
    /// degrade to "disconnect only" / "always false" rather than crash.
    /// `FireflyModel` already owns `SettingsStoring` (this file lives in
    /// the same module), so this is a direct dependency, not a closure
    /// like `BLETransport.onPreferredPeripheralChanged` has to be
    /// (`FireflyMesh` cannot depend on `FireflyModel` — that closure's
    /// own doc comment).
    private let store: (any SettingsStoring)?
    private var observation: Task<Void, Never>?
    /// M-Connect-UX (owner feedback: "not sure to which radio") —
    /// mirrors `client.nodeUpdates()` for exactly one purpose: filling
    /// in `connectedRadio.longName` once the connected node's own
    /// `NodeInfo` arrives. A SEPARATE subscription from `observation`
    /// above (own `Task`, own idempotency), the same "each consumer
    /// gets its own stream" rule `NearbyNodesViewModel.observe()`
    /// follows — this one must never starve, or be starved by, that
    /// screen's own `nodeUpdates()` subscription.
    private var nodeObservation: Task<Void, Never>?
    /// Injectable so `lastConnectedLabel`'s "X ago" arithmetic is
    /// testable without a real wall-clock wait — same convention
    /// `MeshtasticClient.renderedDeliveryState(...)` uses.
    private let now: () -> Date

    public init(client: any MeshtasticClientProtocol, store: (any SettingsStoring)? = nil,
                now: @escaping () -> Date = Date.init) {
        self.client = client
        self.store = store
        self.now = now
    }

    /// Stop mirroring. Not a `deinit`: this type is `@MainActor`, and
    /// a `deinit` cannot touch main-actor state under strict
    /// concurrency. The SwiftUI shell calls this from `.onDisappear`.
    public func stopObserving() {
        guard observation != nil else { return }
        Self.log("stopObserving(): cancelling the linkState() subscription")
        observation?.cancel()
        observation = nil
        nodeObservation?.cancel()
        nodeObservation = nil
    }

    /// Start mirroring the client's link state. Idempotent.
    ///
    /// `client.linkState()` is called HERE, synchronously, rather than
    /// inside the `Task` below: `EventHub.subscribe()` (what backs it)
    /// registers the subscription the instant it is called, and a value
    /// yielded before a subscriber exists is simply missed — multicast,
    /// not replayed. Capturing the stream before the `Task` is created
    /// guarantees the subscription is live before `connect()` can
    /// publish anything, regardless of how the cooperative pool happens
    /// to schedule the `Task`.
    public func observe() {
        guard observation == nil else {
            Self.log("observe(): already observing — no-op")
            return
        }
        Self.log("observe(): subscribing to client.linkState()")
        let stream = client.linkState()
        observation = Task { [weak self] in
            for await state in stream {
                guard let self else { return }
                Self.log("observe(): linkState() stream yielded \(state)")
                self.apply(state)
            }
            Self.log("observe(): linkState() stream ended")
        }
        let nodeStream = client.nodeUpdates()
        nodeObservation = Task { [weak self] in
            for await snapshot in nodeStream {
                guard let self else { return }
                self.apply(snapshot)
            }
        }
    }

    public func connect() async {
        Self.log("connect() called — current link=\(link)")
        lastError = nil
        do {
            try await client.connect()
            Self.log("connect(): client.connect() returned successfully (link=\(link))")
        } catch MeshtasticClientError.alreadyConnecting {
            Self.log("connect(): client.connect() threw .alreadyConnecting — deliberately NOT touching link/lastError; " +
                      "the call already in flight (AppGraph's launch auto-connect, most likely) owns the real outcome")
            // BLOCKING 2 (PR #272 review): `.alreadyConnecting` is a
            // benign race, not a real failure — this call simply lost to
            // an already in-flight `connect()` (`AppGraph`'s launch
            // auto-connect racing a user's own CONNECT tap on cold
            // launch, or an ordinary fast double-tap before
            // `isBusyOrConnected` has propagated away from
            // `.disconnected`). Deliberately do NOT touch `link` or
            // `lastError` here: forcing `.failed("alreadyConnecting")`
            // would show a bogus FAILED for what is actually an
            // in-progress, successful connect — the WINNING call's own
            // `linkState()` stream events (already subscribed via
            // `observe()`) are what report the real outcome.
        } catch {
            Self.log("connect(): client.connect() threw \(error) — publishing .failed")
            lastError = String(describing: error)
            link = .failed(String(describing: error))
        }
    }

    /// Same discipline as `BLETransport.log(_:)`/`MeshtasticClient.log(_:)`
    /// — a raw stderr write, unconditional: this is the ONE place a
    /// user's own CONNECT tap enters the live graph, and it had NO
    /// logging at all before this (the app: fix live connect path
    /// investigation's own finding — `BLETransport` and, now,
    /// `MeshtasticClient` both log every step of their half of a
    /// connect; the view-model half that decides what the button
    /// actually DOES with the result had none).
    private static func log(_ message: String) {
        let line = "[ConnectViewModel] \(message)\n"
        FileHandle.standardError.write(Data(line.utf8))
    }

    /// SHOULD-FIX 5 (PR #272 review): deliberately does NOT clear the
    /// persisted `SettingsKey.lastPeripheralID` — a plain disconnect
    /// keeps the node remembered, so the next cold launch's
    /// `AppGraph.autoConnectToLastKnownPeripheral()` auto-connects back
    /// to it, same as before this method ran. This matches
    /// Meshtastic-Apple's own `AccessoryManager.disconnect()`, which
    /// also never touches `UserDefaults.preferredPeripheralId` — cited
    /// against the actual GPL-3.0 source, not assumed. `forgetNode()`,
    /// below, is the one action that DOES clear it; call that instead
    /// when the intent is "stop auto-connecting to this radio", not
    /// this one.
    public func disconnect() async {
        await client.disconnect()
    }

    /// SHOULD-FIX 5 (PR #272 review) — "Forget this node": clears the
    /// persisted `SettingsKey.lastPeripheralID` AND disconnects, so a
    /// relaunch does not silently auto-connect back to this radio
    /// (`disconnect()`'s own doc comment covers the plain-DISCONNECT
    /// case this is distinct from). A no-op on the store side when this
    /// view model was built with none (`store == nil`) — disconnecting
    /// still happens either way.
    public func forgetNode() async {
        store?.setString(nil, .lastPeripheralID)
        await disconnect()
    }

    /// Nothing to forget when nothing is remembered (`lastPeripheralID`
    /// unset) or when this view model has no settings seam at all
    /// (`store == nil` — the M1 stub composition and every plain
    /// `ConnectViewModel(client:)` test). The Connect screen's FORGET
    /// action disables itself on this rather than always being tappable
    /// dead chrome.
    public var canForgetNode: Bool {
        store?.string(.lastPeripheralID) != nil
    }

    /// Exposed for tests and for the stream consumer; keeps the
    /// `.failed` -> `lastError` rule in one place.
    public func apply(_ state: LinkState) {
        link = state
        switch state {
        case .failed(let message): lastError = message
        case .ready: lastConnectedAt = now()
        // A fully dropped link has no "which radio" to keep naming —
        // the honest reset is back to plain NOT CONNECTED, same as
        // before any radio was ever picked. `.failed`/`.reconnecting`
        // deliberately do NOT hit this branch: those are still ABOUT a
        // specific radio (worth naming while retrying, or while telling
        // the user what just failed), unlike a clean `.disconnected`.
        case .disconnected: connectedRadio = nil
        default: break
        }
    }

    /// Called by the Connect screen the moment CONNECT is tapped on a
    /// specific NEARBY RADIOS row — the only way this view model can
    /// learn a BLE advertised name at all (it holds no BLE stack of its
    /// own; `PeripheralDiscovery.swift`'s scan lives one layer up, in
    /// the app target). Fires BEFORE `connect()` so the header can name
    /// the radio through the whole CONNECTING/HANDSHAKING window, not
    /// only once `.ready` — matching the owner's ask ("while
    /// handshaking: 'CONNECTING · Meshtastic_e7d4'"). Resets `longName`
    /// too: a fresh selection is a DIFFERENT radio, and carrying over a
    /// previous one's node identity here would misname this one until
    /// its own `NodeInfo` arrives.
    public func noteSelectedPeripheral(name: String?, rssiDbm: Int?) {
        connectedRadio = ConnectedRadioSummary(bleName: name, longName: nil, rssiDbm: rssiDbm)
    }

    /// Node-identity half of `connectedRadio` — mirrors
    /// `NearbyNodesViewModel.apply(_:MeshNodeSnapshot)`'s naming.
    /// Ignores every snapshot except the CONNECTED node's own: a
    /// stranger's `NodeInfo` arriving mid-scan must never overwrite
    /// "which radio am I on" with somebody else's name. Blank names
    /// (the radio has no owner-set long name yet) are left nil rather
    /// than shown as an empty bullet.
    public func apply(_ snapshot: MeshNodeSnapshot) {
        guard snapshot.num == client.connectedNodeNum else { return }
        guard let longName = snapshot.longName, !longName.isEmpty else { return }
        var radio = connectedRadio ?? ConnectedRadioSummary()
        radio.longName = longName
        connectedRadio = radio
    }

    /// `!%08x`-formatted node id — Meshtastic's own convention
    /// (`MeshNodeSnapshot.num`'s doc comment; `NearbyRow`'s hex
    /// fallback uses the same format). Sourced from
    /// `client.connectedNodeNum` directly rather than waiting on a
    /// `NodeInfo` snapshot for the connected node: `connectedNodeNum`
    /// is synchronous and always correct the moment `my_info` lands
    /// (protocol doc comment), so this is available even when the
    /// radio never reports its own `NodeInfo` in the dump — true of
    /// every demo-mode connect (`DemoWorld.nodeDB` deliberately excludes
    /// "my" own node — see that file's header) and briefly true on real
    /// hardware too, right up until the nodeDB phase gets to it. nil
    /// once the link drops — an id from a session that just ended is
    /// not "which radio am I on" any more.
    public var connectedNodeIDHex: String? {
        guard link != .disconnected, let num = client.connectedNodeNum else { return nil }
        return String(format: "!%08x", num)
    }

    /// The Connect screen header's ENTIRE state line, one property so
    /// the view renders it verbatim (MVVM convention 5) — "CONNECTED ·
    /// Meshtastic_e7d4 · Firefly 2 · !02e5e3d4 · −56 dBm", trimmed down
    /// to whatever is actually known (`statusLabel` alone before any
    /// radio is picked; `statusLabel · bleName` through most of a
    /// CONNECTING/HANDSHAKING window). Never pads a missing piece with
    /// a placeholder — an unknown long name is a shorter line, not a
    /// blank bullet.
    public var headerStatusText: String {
        var parts = [statusLabel]
        if let bleName = connectedRadio?.bleName { parts.append(bleName) }
        if let longName = connectedRadio?.longName { parts.append(longName) }
        if let nodeID = connectedNodeIDHex { parts.append(nodeID) }
        if let rssi = connectedRadio?.rssiDbm { parts.append("\(rssi) dBm") }
        return parts.joined(separator: " · ")
    }

    /// `SettingsKey.lastPeripheralID`, spelled out for the Connect
    /// screen's row builder — the same value `canForgetNode` already
    /// reads, just as the id string itself rather than a Bool, so a
    /// NEARBY RADIOS row can tell "this is the one I'm remembering"
    /// apart from every other discovered peripheral.
    public var rememberedPeripheralID: String? {
        store?.string(.lastPeripheralID)
    }

    /// Owner feedback: "the connect button needs to be on the line item
    /// or something" — this is the per-row gating table.
    /// `isActivePeripheral` is true for exactly the ONE row a screen's
    /// row-builder considers "the" radio (remembered, connecting, or
    /// connected — `RadioListBuilder`'s own doc comment); every other
    /// row is `false`. This is deliberately the SAME state mapping
    /// `isDisconnectable` above already pins (SHOULD-FIX 3, PR #272
    /// review): DISCONNECT must stay reachable on the active row for
    /// the WHOLE `.connecting`/`.handshaking`/`.reconnecting` window,
    /// not only once `.ready` — a bounded retry loop can run for
    /// minutes, and a user with a good reason to bail needs an abort
    /// the entire time. Every OTHER row's CONNECT goes `.unavailable`
    /// for that same window — this app can only ever be talking to one
    /// radio at a time (`BLETransport`/`MeshtasticClient`, singular).
    /// `.failed` reopens CONNECT on every row (including the one that
    /// just failed) — matching `connectButtonLabel`'s own RETRY rule.
    public enum RadioRowAction: Equatable, Sendable {
        case connect
        case disconnect
        case unavailable
    }
    public func rowAction(isActivePeripheral: Bool) -> RadioRowAction {
        switch link {
        case .disconnected, .failed:
            return .connect
        case .connecting, .handshaking, .ready, .reconnecting:
            return isActivePeripheral ? .disconnect : .unavailable
        }
    }

    /// What the Connect screen puts under the button. Deliberately says
    /// HANDSHAKING rather than CONNECTED during the config dump: the
    /// nodeDB is not trustworthy until `config_complete_id` matches, and
    /// a screen that said "connected" there would be showing an empty
    /// crew as if it were the answer.
    public var statusLabel: String {
        switch link {
        case .disconnected: return "NOT CONNECTED"
        case .connecting: return "CONNECTING"
        case .handshaking: return "HANDSHAKING"
        case .ready: return "CONNECTED"
        case .reconnecting(let attempt): return "RECONNECTING (attempt \(attempt))"
        case .failed: return "FAILED"
        }
    }

    /// NIT (PR #272 review): before this, the terminal state after the
    /// bounded handshake-retry loop gives up (`.failed`, once
    /// `handshakeRetryLimit` attempts are spent — `MeshtasticClient
    /// .handleTransportReconnected()`) had no action of its own — the
    /// CONNECT button was merely re-enabled (`isBusyOrConnected` already
    /// reads `false` for `.failed`), with nothing telling the user that
    /// tapping it again is exactly the right move. "RETRY" makes that
    /// terminal state's own next step visible rather than silently
    /// relying on the button's ordinary label to double as one.
    public var connectButtonLabel: String {
        if case .failed = link { return "RETRY" }
        return "CONNECT"
    }

    /// M2 — "'last connected X ago'": nil while `.ready` (there is
    /// nothing to say — it IS connected) or before any `.ready` has ever
    /// been observed; a short relative-time string otherwise, so the
    /// Connect screen can say something honest about a link that is
    /// reconnecting or has dropped rather than just "NOT CONNECTED" with
    /// no further context.
    public var lastConnectedLabel: String? {
        guard link != .ready, let lastConnectedAt else { return nil }
        return "last connected \(Self.relativeAgo(from: lastConnectedAt, to: now()))"
    }

    /// M2: `.reconnecting` joins the already-busy states — a manual
    /// CONNECT tap while the client is mid-backoff-retry would race
    /// `MeshtasticClient`'s own reentrancy guard
    /// (`MeshtasticClientError.alreadyConnecting`) for nothing. Moved
    /// here (PR #272 review, SHOULD-FIX 3) from a private computed var
    /// on `ConnectScreen` itself so the CONNECT/DISCONNECT gating state
    /// matrix is unit-testable without SwiftUI, the same way
    /// `statusLabel`/`lastConnectedLabel` already are.
    public var isBusyOrConnected: Bool {
        switch link {
        case .ready, .connecting, .handshaking, .reconnecting: return true
        case .disconnected, .failed: return false
        }
    }

    /// SHOULD-FIX 3 (PR #272 review): before this, DISCONNECT was gated
    /// on `link == .ready`, so it — together with `isBusyOrConnected`
    /// gating CONNECT — was unreachable for the ENTIRE `.connecting`/
    /// `.handshaking`/`.reconnecting` window. Pre-M2 that window was one
    /// short handshake attempt; M2's bounded retry loop can legitimately
    /// run for minutes (`handshakeRetryLimit` attempts, each budgeted up
    /// to `configPhaseTimeout` + `nodeDBPhaseTimeout` before its own
    /// backoff sleep even starts), during which a user with a good
    /// reason to bail — wrong node still connected, switching devices,
    /// saving battery — had no way to abort it. DISCONNECT is reachable
    /// any time the link is not already `.disconnected`:
    /// `MeshtasticClient.disconnect()` cancels `reconnectTask` (and
    /// `receiveTask`/`heartbeatTask`) unconditionally, so this is always
    /// a real abort, not a no-op. `.failed` is excluded — nothing is
    /// running there to cancel, so DISCONNECT would just be dead chrome
    /// on an already-terminal state.
    public var isDisconnectable: Bool {
        switch link {
        case .disconnected, .failed: return false
        case .ready, .connecting, .handshaking, .reconnecting: return true
        }
    }

    /// Pure and testable with no real wall-clock wait. Coarse on
    /// purpose — this is "roughly how long", not a stopwatch.
    public static func relativeAgo(from date: Date, to now: Date) -> String {
        let seconds = max(0, Int(now.timeIntervalSince(date)))
        if seconds < 60 { return "\(seconds)s ago" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes)m ago" }
        let hours = minutes / 60
        return "\(hours)h ago"
    }
}
