//
//  PeripheralDiscovery.swift — the Connect screen's "which BLE radio to
//  connect to" seam (docs/specs/A01-companion-app.md's M1 Connect-screen
//  bullet: "node picker (BLE on both platforms)").
//
//  INTEGRATED: slice A's `BLETransport` has landed, and with it a real
//  scan (`FireflyMesh.NodeScanning`: `scan()` / `stopScanning()` /
//  `setPreferredPeripheral(_:)`). `MeshPeripheralDiscovery` below is
//  this screen's adapter onto it — the "real scan-result list is exactly
//  the kind of thing a human integrator wires up once slice A lands"
//  this file's original comment described.
//
//  `StubPeripheralDiscovery` stays for the two cases that genuinely have
//  no radio — the iOS Simulator (`AppDependencies.stub()`, whose
//  `scanner` is nil) and `PeripheralDiscoveryTests` — and keeps its
//  defining property: it discovers nothing, and an empty picker is the
//  honest answer rather than an invented peripheral.
//
import FireflyMesh
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
    /// follows (`EventHub`, S1).
    func peripherals() -> AsyncStream<[DiscoveredPeripheral]>
    /// Make this peripheral the one a subsequent `connect()` prefers.
    /// The picker's whole point: without it, `connect()` takes whatever
    /// the transport's own internal scan sees first, which on a bench
    /// with two boards advertising is a coin flip.
    func select(_ id: String)
}

/// Discovers nothing — the honest answer wherever there is no radio at
/// all (the iOS Simulator; `AppDependencies.stub()` carries no
/// `scanner`). Matches `StubMeshtasticClient`'s own defining property:
/// it refuses to invent data it does not have, so an empty picker here
/// is the same honest answer an empty Radar is elsewhere in this app.
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

    /// Nothing was ever discovered, so there is nothing to select and no
    /// transport to tell. Not an error — the picker simply has no rows
    /// to tap.
    func select(_ id: String) {}
}

/// The real one: `NodeScanning` (slice A's `BLETransport`) behind this
/// screen's own vocabulary.
///
/// Accumulates discoveries into a de-duplicated, RSSI-sorted list rather
/// than passing each sighting straight through: `scan()` yields ONE
/// `BLEDiscoveredPeripheral` per advertisement, and the same board
/// advertises every few hundred milliseconds, so a picker fed raw
/// sightings would be a flickering list of duplicates. The RSSI shown is
/// always the most recent sighting's — a board that moves closer must
/// not keep rendering the reading from when it was across the room.
@MainActor
final class MeshPeripheralDiscovery: PeripheralDiscovering {
    private let scanner: any NodeScanning
    private let hub = PeripheralListHub()
    private var scanTask: Task<Void, Never>?
    private var byID: [UUID: BLEDiscoveredPeripheral] = [:]

    init(scanner: any NodeScanning) {
        self.scanner = scanner
    }

    func startScanning() {
        // Idempotent, like every `observe()` in this app: RESCAN on an
        // already-running scan republishes what is known rather than
        // opening a second subscription to the same transport.
        guard scanTask == nil else {
            hub.publish(Self.sorted(byID))
            return
        }
        scanTask = Task { [weak self, scanner] in
            let stream = await scanner.scan()
            for await peripheral in stream {
                guard let self else { return }
                self.record(peripheral)
            }
        }
    }

    func stopScanning() {
        scanTask?.cancel()
        scanTask = nil
        Task { [scanner] in await scanner.stopScanning() }
    }

    func peripherals() -> AsyncStream<[DiscoveredPeripheral]> { hub.subscribe() }

    func select(_ id: String) {
        guard let uuid = UUID(uuidString: id) else { return }
        Task { [scanner] in await scanner.setPreferredPeripheral(uuid) }
    }

    private func record(_ peripheral: BLEDiscoveredPeripheral) {
        byID[peripheral.id] = peripheral
        hub.publish(Self.sorted(byID))
    }

    /// Strongest first — the board on the table should be the first row,
    /// not the one three tents over. Ties break on the identifier so the
    /// list cannot jitter between two equal readings.
    static func sorted(_ byID: [UUID: BLEDiscoveredPeripheral]) -> [DiscoveredPeripheral] {
        byID.values
            .sorted { ($0.rssi, $0.id.uuidString) > ($1.rssi, $1.id.uuidString) }
            .map { DiscoveredPeripheral(id: $0.id.uuidString, name: $0.name, rssiDbm: $0.rssi) }
    }
}

/// A tiny multicast hub for the picker's list, so `peripherals()` can
/// honour the same "a fresh, independent stream for the caller" contract
/// the rest of the app's streams do. `FireflyMesh.EventHub` is the same
/// idea, but the list is state this screen owns rather than an event the
/// mesh publishes.
@MainActor
private final class PeripheralListHub {
    private var continuations: [UUID: AsyncStream<[DiscoveredPeripheral]>.Continuation] = [:]
    private var latest: [DiscoveredPeripheral] = []

    func subscribe() -> AsyncStream<[DiscoveredPeripheral]> {
        AsyncStream(bufferingPolicy: .bufferingNewest(8)) { continuation in
            let id = UUID()
            continuations[id] = continuation
            // Unlike an event stream, a LIST is state: a subscriber that
            // arrives after the scan started needs what is already known,
            // not silence until the next advertisement.
            continuation.yield(latest)
            continuation.onTermination = { [weak self] _ in
                Task { @MainActor in self?.continuations[id] = nil }
            }
        }
    }

    func publish(_ list: [DiscoveredPeripheral]) {
        latest = list
        for continuation in continuations.values { continuation.yield(list) }
    }
}
