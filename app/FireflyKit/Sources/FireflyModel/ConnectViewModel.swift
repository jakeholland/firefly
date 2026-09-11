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

    private let client: any MeshtasticClientProtocol
    private var observation: Task<Void, Never>?

    public init(client: any MeshtasticClientProtocol) {
        self.client = client
    }

    /// Stop mirroring. Not a `deinit`: this type is `@MainActor`, and
    /// a `deinit` cannot touch main-actor state under strict
    /// concurrency. The SwiftUI shell calls this from `.onDisappear`.
    public func stopObserving() {
        observation?.cancel()
        observation = nil
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
        guard observation == nil else { return }
        let stream = client.linkState()
        observation = Task { [weak self] in
            for await state in stream {
                guard let self else { return }
                self.apply(state)
            }
        }
    }

    public func connect() async {
        lastError = nil
        do {
            try await client.connect()
        } catch {
            lastError = String(describing: error)
            link = .failed(String(describing: error))
        }
    }

    public func disconnect() async {
        await client.disconnect()
    }

    /// Exposed for tests and for the stream consumer; keeps the
    /// `.failed` -> `lastError` rule in one place.
    public func apply(_ state: LinkState) {
        link = state
        if case .failed(let message) = state { lastError = message }
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
        case .failed: return "FAILED"
        }
    }
}
