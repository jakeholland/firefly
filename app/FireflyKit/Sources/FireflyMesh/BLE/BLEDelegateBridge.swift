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
//  A03 S1b / §3.1: the hop is no longer a bare, independent `Task` per
//  callback. Every method below goes through `BLEDelegateDelivery`,
//  which chains the hops so the actor sees callbacks in CoreBluetooth's
//  own delivery order — and `willRestoreState` raises its restore
//  marker synchronously on the delegate queue before hopping at all.
//  `BLEDelegateDelivery`'s own header is where the race that forces
//  both halves is written up.
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
    /// A03 §3.1 — the ordering fix. Every callback below is delivered
    /// through this, so the actor sees them in the order CoreBluetooth
    /// delivered them; `willRestoreState` additionally raises its
    /// restore marker SYNCHRONOUSLY here, before any hop exists. See
    /// `BLEDelegateDelivery`'s own header for why both halves are
    /// needed and what breaks without them.
    let delivery: BLEDelegateDelivery

    init(transport: BLETransport, delivery: BLEDelegateDelivery) {
        self.transport = transport
        self.delivery = delivery
    }

    /// The one hop. `transport` is hoisted into a local `let` BEFORE the
    /// closure is built, for the reason this file's header gives:
    /// capturing `self` (a plain, non-`Sendable` `NSObject`) into a
    /// `@Sendable` closure is what strict concurrency flags; capturing
    /// the local actor reference is not.
    private func deliver(_ work: @escaping @Sendable (BLETransport) async -> Void) {
        guard let transport else { return }
        delivery.enqueue { await work(transport) }
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let state = central.state
        deliver { await $0.handleCentralStateUpdate(state) }
    }

    /// M2 — CoreBluetooth state restoration
    /// (`BLETransport.ensureCentralManagerExists`'s own doc comment).
    /// Only fires on iOS, and only when the manager was created with
    /// `CBCentralManagerOptionRestoreIdentifierKey`.
    ///
    /// A03 §3.1 — `markRestorePending()` is called FIRST, synchronously,
    /// on the delegate queue, before the hop below is built and before
    /// this method returns to CoreBluetooth. That is the half of the
    /// ordering fix which does not depend on task scheduling at all: the
    /// delegate queue is serial, so every later callback on it — the
    /// `didUpdateState` Apple delivers right after this one — sees the
    /// marker already set.
    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        delivery.markRestorePending()
        let peripherals = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral]) ?? []
        let crossing = CoreBluetoothCrossing(value: peripherals)
        deliver { await $0.handleWillRestoreState(peripherals: crossing.value) }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                         advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name
        let crossing = CoreBluetoothCrossing(value: peripheral)
        let rssi = RSSI.intValue
        deliver { await $0.handleDiscovered(peripheral: crossing.value, name: name, rssi: rssi) }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        deliver { await $0.handleConnected(peripheral: crossing.value) }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        deliver { await $0.handleFailedToConnect(peripheral: crossing.value, error: error) }
    }

    /// The legacy 2-argument callback. iOS calls the 5-argument one
    /// below instead once it is implemented (**[community]**, A03 §1.4:
    /// "whether implementing the 5-argument delegate suppresses the
    /// legacy 2-argument one is [community] (reported: yes). Implement
    /// both."), so this stays for macOS and as a fallback.
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        deliver { await $0.handleDisconnected(peripheral: crossing.value, error: error) }
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
        let disconnectedAt = Date(timeIntervalSinceReferenceDate: timestamp)
        deliver {
            await $0.handleDisconnected(peripheral: crossing.value, disconnectedAt: disconnectedAt,
                                        isReconnecting: isReconnecting, error: error)
        }
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        let crossing = CoreBluetoothCrossing(value: peripheral)
        deliver { await $0.handleDiscoveredServices(peripheral: crossing.value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: service)
        deliver { await $0.handleDiscoveredCharacteristics(service: crossing.value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                     error: Error?) {
        let crossing = CoreBluetoothCrossing(value: characteristic)
        deliver { await $0.handleNotificationStateUpdate(characteristic: crossing.value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        let value = characteristic.value
        let crossing = CoreBluetoothCrossing(value: characteristic)
        deliver { await $0.handleValueUpdate(characteristic: crossing.value, value: value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        let crossing = CoreBluetoothCrossing(value: characteristic)
        deliver { await $0.handleWriteConfirmation(characteristic: crossing.value, error: error) }
    }
}
