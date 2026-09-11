//
//  DiagnosticsViewModel.swift — the Diagnostics sub-screen: live values
//  read from the node only, never inferred (docs/specs/
//  A01-companion-app.md, Design language > Diagnostics; M1 acceptance:
//  "Diagnostics shows link state, frame counters and firmware version,
//  and shows 'unknown' where it does not know").
//
//  A THIRD independent `linkState()` subscription — the spec calls this
//  out explicitly ("for linkState also Diagnostics") alongside
//  ConnectViewModel's and CoreStore's own, all three backed by the same
//  EventHub multicast (S1) without stealing each other's events.
//
import FireflyMesh
import Foundation
import Observation

@MainActor
@Observable
final class DiagnosticsViewModel {
    private(set) var link: LinkState = .disconnected

    private let client: any MeshtasticClientProtocol
    private var observation: Task<Void, Never>?

    init(client: any MeshtasticClientProtocol) {
        self.client = client
    }

    func observe() {
        guard observation == nil else { return }
        let stream = client.linkState()
        observation = Task { [weak self] in
            for await state in stream {
                guard let self else { return }
                self.link = state
            }
        }
    }

    func stopObserving() {
        observation?.cancel()
        observation = nil
    }

    var linkStateLabel: String {
        switch link {
        case .disconnected: return "NOT CONNECTED"
        case .connecting: return "CONNECTING"
        case .handshaking: return "HANDSHAKING"
        case .ready: return "CONNECTED"
        case .failed: return "FAILED"
        }
    }

    /// Every field below needs a value `MeshtasticClientProtocol` does
    /// not expose in M1 — no packet counters, no battery/voltage
    /// telemetry, no firmware-version field on the seam at all. UNKNOWN
    /// is the honest rendering the acceptance criterion asks for, not
    /// a placeholder number standing in for a real one.
    static let unknown = "UNKNOWN"

    let heardInLast10MinCount = DiagnosticsViewModel.unknown
    let packetsIn = DiagnosticsViewModel.unknown
    let packetsOut = DiagnosticsViewModel.unknown
    let ackRate = DiagnosticsViewModel.unknown
    let nodeBatteryPercent = DiagnosticsViewModel.unknown
    let nodeVoltage = DiagnosticsViewModel.unknown
    let firmwareVersion = DiagnosticsViewModel.unknown
}
