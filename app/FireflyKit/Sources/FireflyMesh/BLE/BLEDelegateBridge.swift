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
import Foundation
@preconcurrency import CoreBluetooth

final class BLEDelegateBridge: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    weak var transport: BLETransport?

    init(transport: BLETransport) {
        self.transport = transport
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { await transport?.handleCentralStateUpdate(central.state) }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                         advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name
        Task { await transport?.handleDiscovered(peripheral: peripheral, name: name, rssi: RSSI.intValue) }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { await transport?.handleConnected(peripheral: peripheral) }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        Task { await transport?.handleFailedToConnect(peripheral: peripheral, error: error) }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        Task { await transport?.handleDisconnected(peripheral: peripheral, error: error) }
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        Task { await transport?.handleDiscoveredServices(peripheral: peripheral, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        Task { await transport?.handleDiscoveredCharacteristics(service: service, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                     error: Error?) {
        Task { await transport?.handleNotificationStateUpdate(characteristic: characteristic, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        let value = characteristic.value
        Task { await transport?.handleValueUpdate(characteristic: characteristic, value: value, error: error) }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        Task { await transport?.handleWriteConfirmation(characteristic: characteristic, error: error) }
    }
}
