//
//  BLEDelegateBridge.swift — the NSObject glue CoreBluetooth requires.
//
//  `CBCentralManagerDelegate`/`CBPeripheralDelegate` methods are plain,
//  synchronous, `NSObjectProtocol`-bound callbacks — an `actor` cannot
//  conform to them directly. This tiny class exists ONLY to receive
//  those callbacks and hop them onto `BLETransport`'s actor via `Task`,
//  the same shape Meshtastic-Apple's own `BLEConnectionDelegate` uses
//  (`Meshtastic/Accessory/Transports/Bluetooth Low Energy/
//  BLEConnection.swift`: `func peripheral(...) { Task { await
//  connection?.didDiscoverCharacteristicsFor(...) } }`) — cited as the
//  second independent confirmation of the GATT contract itself in
//  `MeshtasticBLE.swift`, and reused here for the delegate SHAPE too.
//  Holds `transport` `weak` so a torn-down transport cannot be kept
//  alive by CoreBluetooth's own strong reference to its delegate.
//
//  M3 / Swift 6 strict concurrency: every method below hoists
//  `transport` into a local `let` BEFORE building the `Task` — capturing
//  `self` (a plain, non-Sendable `NSObject`) into a `Task`'s `@Sendable`
//  closure is what strict concurrency flags; capturing the local actor
//  reference instead (actor types are implicitly `Sendable`) is not.
//  `CBPeripheral`/`CBService`/`CBCharacteristic` are a separate problem:
//  CoreBluetooth predates `Sendable` and never annotated them, but the
//  framework's own contract — every delegate callback for a given
//  central/peripheral is delivered serially on the single queue passed
//  to `CBCentralManager(delegate:queue:)` (`BLETransport`'s
//  `ensureCentralManagerExists`, unchanged by this slice) — is exactly
//  the guarantee an actor hop needs: at most one hop is ever in flight
//  for a given object. `CoreBluetoothCrossing` below documents and
//  contains that one boundary-crossing `@unchecked Sendable`, rather
//  than sprinkling the annotation across call sites.
//
import Foundation
@preconcurrency import CoreBluetooth

/// Carries a single CoreBluetooth object (or array of them) across the
/// one hop this file exists to make: delegate-queue callback ->
/// `BLETransport` actor. `@unchecked` because CoreBluetooth's own types
/// predate `Sendable` — NOT because anything here is actually shared,
/// mutable state, and NOT because the box enforces which queue/executor
/// ever touches the value afterward (PR #275 review, SHOULD-FIX 5 — an
/// earlier version of this comment oversold that guarantee).
///
/// The box's own job is narrow: it exists purely to satisfy the
/// Sendable-closure check on the `Task { ... }` these callbacks build —
/// handing a non-`Sendable` CoreBluetooth reference to `Task` directly
/// is what strict concurrency flags, and this is a documented, narrowly-
/// scoped way to say "this specific value is fine to cross." The reason
/// it is ACTUALLY fine to use afterward is CoreBluetooth's own thread-
/// safety contract for its instance methods (`CBPeripheral.writeValue`/
/// `.discoverServices`/`.discoverCharacteristics`/`.setNotifyValue`/
/// `.readValue`, `CBCentralManager.connect`, etc.): Apple documents these
/// as safe to call from ANY thread or queue, not only the one passed to
/// `CBCentralManager(delegate:queue:)` — that queue governs DELEGATE
/// CALLBACK delivery (serialized, one at a time, which is what makes
/// each `BLEDelegateBridge` method itself simple and non-reentrant), not
/// which thread may call INTO CoreBluetooth. `BLETransport` (a plain
/// `actor`, no custom executor pinned to that queue —
/// `ensureCentralManagerExists`'s own doc comment) calling those methods
/// from its own actor executor, a different execution context than the
/// delegate queue, is exactly this documented "any thread" contract in
/// use, not an incidental accident this box happens to paper over.
/// Never use this for state this file (or its callers) mutate from more
/// than one place — that would be the "real shared mutable state" case
/// `nonisolated(unsafe)`/`@unchecked Sendable` must not be used for.
private struct CoreBluetoothCrossing<Value>: @unchecked Sendable {
    let value: Value
}

final class BLEDelegateBridge: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    weak var transport: BLETransport?

    init(transport: BLETransport) {
        self.transport = transport
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let transport = transport
        let state = central.state
        Task { await transport?.handleCentralStateUpdate(state) }
    }

    /// M2 — CoreBluetooth state restoration
    /// (`BLETransport.ensureCentralManagerExists`'s own doc comment).
    /// Only fires on iOS, and only when the manager was created with
    /// `CBCentralManagerOptionRestoreIdentifierKey`.
    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        let peripherals = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral]) ?? []
        let crossing = CoreBluetoothCrossing(value: peripherals)
        let transport = transport
        Task { await transport?.handleWillRestoreState(peripherals: crossing.value) }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                         advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name
        let crossing = CoreBluetoothCrossing(value: peripheral)
        let rssi = RSSI.intValue
        let transport = transport
        Task { await transport?.handleDiscovered(peripheral: crossing.value, name: name, rssi: rssi) }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        let transport = transport
        Task { await transport?.handleConnected(peripheral: crossing.value) }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        let transport = transport
        Task { await transport?.handleFailedToConnect(peripheral: crossing.value, error: error) }
    }

    /// The legacy 2-argument callback. iOS calls the 5-argument one
    /// below instead once it is implemented (**[community]**, A03 §1.4:
    /// "whether implementing the 5-argument delegate suppresses the
    /// legacy 2-argument one is [community] (reported: yes). Implement
    /// both."), so this stays for macOS and as a fallback.
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        let transport = transport
        Task { await transport?.handleDisconnected(peripheral: crossing.value, error: error) }
    }

    /// A03 §3.4 — the iOS 17 / macOS 14 disconnect delegate.
    ///
    /// `isReconnecting` is whether the central manager will itself
    /// attempt to reconnect (so we must NOT), and `timestamp` is when
    /// the disconnection actually occurred — which matters precisely
    /// because it may have happened while the app was suspended, and a
    /// `Date()` taken here would be the time we WOKE, not the time we
    /// lost the link (§1.7). `CFAbsoluteTime` is seconds since the 2001
    /// reference date, so `Date(timeIntervalSinceReferenceDate:)` is the
    /// exact, lossless conversion.
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                         timestamp: CFAbsoluteTime, isReconnecting: Bool, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        let transport = transport
        let disconnectedAt = Date(timeIntervalSinceReferenceDate: timestamp)
        Task {
            await transport?.handleDisconnected(peripheral: crossing.value, disconnectedAt: disconnectedAt,
                                                 isReconnecting: isReconnecting, error: error)
        }
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        let transport = transport
        Task { await transport?.handleDiscoveredServices(peripheral: crossing.value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: service)
        let transport = transport
        Task { await transport?.handleDiscoveredCharacteristics(service: crossing.value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                     error: Error?) {
        let crossing = CoreBluetoothCrossing(value: characteristic)
        let transport = transport
        Task { await transport?.handleNotificationStateUpdate(characteristic: crossing.value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        let value = characteristic.value
        let crossing = CoreBluetoothCrossing(value: characteristic)
        let transport = transport
        Task { await transport?.handleValueUpdate(characteristic: crossing.value, value: value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: characteristic)
        let transport = transport
        Task { await transport?.handleWriteConfirmation(characteristic: crossing.value, error: error) }
    }
}
