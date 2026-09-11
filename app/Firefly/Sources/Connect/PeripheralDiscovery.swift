//
//  PeripheralDiscovery.swift — the Connect screen's "which BLE radio to
//  connect to" seam (docs/specs/A01-companion-app.md's M1 Connect-screen
//  bullet: "node picker (BLE on both platforms)").
//
//  MeshtasticClientProtocol (FireflyMesh, landed, not slice-owned) has
//  no scan/discovery surface yet — that belongs to slice A's real BLE
//  transport, being built in parallel. Rather than grow a NEW
//  cross-slice protocol that slice A might independently invent a
//  different shape for, this stays entirely internal to the Connect
//  screen (not exported — nothing outside this file depends on it): a
//  real scan-result list is exactly the kind of thing a human
//  integrator wires up once slice A lands, and until then the honest
//  answer is an empty list, never an invented peripheral — the same
//  rule `StubMeshtasticClient` follows for everything it reports.
//
import Foundation

struct DiscoveredPeripheral: Identifiable, Equatable {
    let id: String
    let name: String?
    /// Raw dBm. Shown next to the peripheral in the picker, same as any
    /// BLE scanner would — this is the discovery-time RSSI, not the
    /// after-connect crew-facing signal tier `SignalTierPresentation`
    /// renders (that vocabulary is for the Nearby section, sourced from
    /// mesh packets once connected, not from advertisement RSSI).
    let rssiDbm: Int
}

protocol PeripheralDiscovering: AnyObject {
    func startScanning()
    func stopScanning()
    /// A fresh, independent stream for the caller, matching the
    /// multicast convention every other event stream in this app
    /// follows (`EventHub`, S1) even though this one has only ever had
    /// a single subscriber so far.
    func peripherals() -> AsyncStream<[DiscoveredPeripheral]>
}

/// Discovers nothing — the only honest answer available before a real
/// BLE scanner exists behind this seam. Matches `StubMeshtasticClient`'s
/// own defining property: it refuses to invent data it does not have,
/// so an empty picker here is the same honest answer an empty Radar is
/// elsewhere in this app.
final class StubPeripheralDiscovery: PeripheralDiscovering {
    private var continuation: AsyncStream<[DiscoveredPeripheral]>.Continuation?

    func startScanning() {
        continuation?.yield([])
    }

    func stopScanning() {}

    func peripherals() -> AsyncStream<[DiscoveredPeripheral]> {
        AsyncStream { continuation in
            self.continuation = continuation
            continuation.yield([])
        }
    }
}
