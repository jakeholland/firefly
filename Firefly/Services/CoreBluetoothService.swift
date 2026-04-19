//
//  CoreBluetoothService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import CoreBluetooth
import Combine
import SwiftProtobuf

/// CoreBluetooth-based implementation of BluetoothServiceProtocol
final class CoreBluetoothService: NSObject, BluetoothServiceProtocol {
    // MARK: - Properties
    
    let connectionState: CurrentValueSubject<BluetoothConnectionState, Never>
    
    private let receivedDataSubject = PassthroughSubject<Data, Never>()
    var receivedDataPublisher: AnyPublisher<Data, Never> {
        receivedDataSubject.eraseToAnyPublisher()
    }
    
    private let discoveredDevicesSubject = CurrentValueSubject<[PeripheralDevice], Never>([])
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> {
        discoveredDevicesSubject.eraseToAnyPublisher()
    }
    
    var discoveredDevices: [PeripheralDevice] {
        discoveredDevicesSubject.value
    }
    
    private var centralManager: CBCentralManager!
    private var connectedPeripheral: CBPeripheral?
    private var toRadioCharacteristic: CBCharacteristic?
    private var fromRadioCharacteristic: CBCharacteristic?
    private var fromNumCharacteristic: CBCharacteristic?
    private var logRadioCharacteristic: CBCharacteristic?
    
    private var pendingSendContinuations: [CheckedContinuation<Void, Error>] = []
    /// Prevents multiple concurrent FROMRADIO reads from piling up.
    /// Only one drain loop runs at a time; further requests are coalesced.
    private var isDrainingFromRadio = false
    
    // MARK: - Initialization
    
    override init() {
        self.connectionState = CurrentValueSubject(.disconnected)
        super.init()
        
        NSLog("🛜 [BLE] Initializing CoreBluetoothService")
        
        // Initialize central manager on main queue
        self.centralManager = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [CBCentralManagerOptionShowPowerAlertKey: true]
        )
    }
    
    // MARK: - Public Methods
    
    func startScanning() {
        NSLog("🛜 [BLE] startScanning() called - Central Manager state: \(cbManagerStateDescription(centralManager.state))")
        
        guard centralManager.state == .poweredOn else {
            NSLog("🛜 [BLE] ❌ Cannot start scanning - centralManager is not powered on (state: \(cbManagerStateDescription(centralManager.state)))")
            connectionState.send(.failed(BluetoothError.deviceNotFound))
            return
        }
        
        NSLog("🛜 [BLE] 📡 Starting scan for Meshtastic devices (service UUID: \(MeshtasticBLE.serviceUUID))")
        connectionState.send(.scanning)
        
        centralManager.scanForPeripherals(
            withServices: [MeshtasticBLE.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true]
        )
    }
    
    func stopScanning() {
        NSLog("🛜 [BLE] 🛑 Stopping scan and clearing discovered devices")
        centralManager.stopScan()
        discoveredDevicesSubject.send([])
    }
    
    func connect(to deviceIdentifier: UUID) {
        NSLog("🛜 [BLE] 🔌 Attempting to connect to device: \(deviceIdentifier.uuidString)")
        stopScanning()
        
        // Find the peripheral from discovered devices or retrieve from system
        let peripheral: CBPeripheral?
        
        if let device = discoveredDevicesSubject.value.first(where: { $0.id == deviceIdentifier }) {
            NSLog("🛜 [BLE] ✅ Found device in discovered devices list: \(device.name)")
            peripheral = device.peripheral
        } else {
            NSLog("🛜 [BLE] 🔍 Device not in discovered list, retrieving from system")
            peripheral = centralManager.retrievePeripherals(withIdentifiers: [deviceIdentifier]).first
            if peripheral != nil {
                NSLog("🛜 [BLE] ✅ Retrieved peripheral from system")
            }
        }
        
        guard let peripheral = peripheral else {
            NSLog("🛜 [BLE] ❌ Failed to find peripheral with identifier: \(deviceIdentifier.uuidString)")
            connectionState.send(.failed(BluetoothError.deviceNotFound))
            return
        }
        
        NSLog("🛜 [BLE] 📱 Peripheral name: \(peripheral.name ?? "Unknown"), state: \(cbPeripheralStateDescription(peripheral.state))")
        
        connectedPeripheral = peripheral
        peripheral.delegate = self
        connectionState.send(.connecting)
        centralManager.connect(peripheral, options: nil)
        
        NSLog("🛜 [BLE] ⏳ Connection initiated, waiting for callback...")
    }
    
    func disconnect() {
        NSLog("🛜 [BLE] 🔌 Disconnect requested")
        if let peripheral = connectedPeripheral {
            NSLog("🛜 [BLE] Cancelling connection to peripheral: \(peripheral.name ?? "Unknown")")
            centralManager.cancelPeripheralConnection(peripheral)
        } else {
            NSLog("🛜 [BLE] No connected peripheral to disconnect from")
        }
        cleanup()
    }
    
    func send(_ data: Data) async throws {
        NSLog("🛜 [BLE] 📤 Attempting to send \(data.count) bytes")
        
        guard let characteristic = toRadioCharacteristic,
              let peripheral = connectedPeripheral else {
            NSLog("🛜 [BLE] ❌ Cannot send - not connected (characteristic: \(toRadioCharacteristic != nil), peripheral: \(connectedPeripheral != nil))")
            throw BluetoothError.notConnected
        }
        
        NSLog("🛜 [BLE] Writing to characteristic \(characteristic.uuid)")
        
        return try await withCheckedThrowingContinuation { continuation in
            pendingSendContinuations.append(continuation)
            peripheral.writeValue(data, for: characteristic, type: .withResponse)
        }
    }
    
    // MARK: - Private Methods
    
    private func readFromRadioQueue() {
        // Coalesce concurrent drain requests — only one read can be in-flight at a time.
        guard !isDrainingFromRadio,
              let peripheral = connectedPeripheral,
              let fromRadio = fromRadioCharacteristic else { return }
        isDrainingFromRadio = true
        peripheral.readValue(for: fromRadio)
    }

    private func cleanup() {
        NSLog("🛜 [BLE] 🧹 Cleaning up connection resources")
        toRadioCharacteristic = nil
        fromRadioCharacteristic = nil
        fromNumCharacteristic = nil
        logRadioCharacteristic = nil
        connectedPeripheral = nil
        isDrainingFromRadio = false
        discoveredDevicesSubject.send([])
        connectionState.send(.disconnected)
        NSLog("🛜 [BLE] ✅ Cleanup complete")
    }
}

// MARK: - CBCentralManagerDelegate

extension CoreBluetoothService: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        NSLog("🛜 [BLE] 🔄 Central Manager state changed to: \(cbManagerStateDescription(central.state))")
        
        switch central.state {
        case .poweredOn:
            NSLog("🛜 [BLE] ✅ Bluetooth is powered on and ready")
            // Re-emit .disconnected so subscribers (e.g. InboxViewModel) know BT
            // is now available and can kick off auto-reconnect if needed.
            // CurrentValueSubject.send() always notifies even when the value is unchanged.
            connectionState.send(.disconnected)
        case .poweredOff:
            NSLog("🛜 [BLE] ⚠️ Bluetooth is powered off")
            connectionState.send(.disconnected)
        case .unauthorized:
            NSLog("🛜 [BLE] ❌ Bluetooth is unauthorized")
            connectionState.send(.failed(BluetoothError.unauthorized))
        case .unsupported:
            NSLog("🛜 [BLE] ❌ Bluetooth is not supported on this device")
            connectionState.send(.failed(BluetoothError.unsupported))
        case .resetting:
            NSLog("🛜 [BLE] 🔄 Bluetooth is resetting")
            connectionState.send(.resetting)
        case .unknown:
            NSLog("🛜 [BLE] ❓ Bluetooth state is unknown")
            connectionState.send(.unknown)
        default:
            break
        }
    }
    
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        let deviceName = peripheral.name ?? "Unknown Device"
        NSLog("🛜 [BLE] 📡 Discovered device: \(deviceName) (\(peripheral.identifier.uuidString)) - RSSI: \(RSSI) dBm")
        
        // Add to discovered devices list
        let device = PeripheralDevice(peripheral: peripheral, rssi: RSSI.intValue)
        var devices = discoveredDevicesSubject.value
        
        // Update existing device or add new one
        if let index = devices.firstIndex(where: { $0.id == device.id }) {
            devices[index] = device
        } else {
            NSLog("🛜 [BLE] ➕ Adding new device to discovered list: \(deviceName)")
            devices.append(device)
        }
        
        discoveredDevicesSubject.send(devices)
        NSLog("🛜 [BLE] 📊 Total discovered devices: \(devices.count)")
    }
    
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        NSLog("🛜 [BLE] ✅ Successfully connected to peripheral: \(peripheral.name ?? "Unknown") (\(peripheral.identifier.uuidString))")
        NSLog("🛜 [BLE] 🔍 Discovering services (looking for \(MeshtasticBLE.serviceUUID))...")
        peripheral.discoverServices([MeshtasticBLE.serviceUUID])
    }
    
    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        let errorMsg = error?.localizedDescription ?? "Unknown error"
        NSLog("🛜 [BLE] ❌ Failed to connect to peripheral: \(peripheral.name ?? "Unknown") - Error: \(errorMsg)")
        connectionState.send(.failed(error ?? BluetoothError.deviceNotFound))
        cleanup()
    }
    
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        if let error = error {
            NSLog("🛜 [BLE] ⚠️ Disconnected from peripheral: \(peripheral.name ?? "Unknown") with error: \(error.localizedDescription)")
        } else {
            NSLog("🛜 [BLE] 🔌 Disconnected from peripheral: \(peripheral.name ?? "Unknown") (normal disconnection)")
        }
        cleanup()
    }
}

// MARK: - CBPeripheralDelegate

extension CoreBluetoothService: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error = error {
            NSLog("🛜 [BLE] ❌ Error discovering services: \(error.localizedDescription)")
            connectionState.send(.failed(BluetoothError.deviceNotFound))
            return
        }
        
        NSLog("🛜 [BLE] 📋 Discovered \(peripheral.services?.count ?? 0) service(s)")
        
        guard let service = peripheral.services?.first(where: { $0.uuid == MeshtasticBLE.serviceUUID }) else {
            NSLog("🛜 [BLE] ❌ Meshtastic service not found in discovered services")
            if let services = peripheral.services {
                for svc in services {
                    NSLog("🛜 [BLE]    - Found service: \(svc.uuid)")
                }
            }
            connectionState.send(.failed(BluetoothError.deviceNotFound))
            return
        }
        
        NSLog("🛜 [BLE] ✅ Found Meshtastic service: \(service.uuid)")
        NSLog("🛜 [BLE] 🔍 Discovering characteristics...")
        
        peripheral.discoverCharacteristics(
            [MeshtasticBLE.toRadioUUID, MeshtasticBLE.fromRadioUUID, MeshtasticBLE.fromNumUUID, MeshtasticBLE.logRadioUUID],
            for: service
        )
    }
    
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error = error {
            NSLog("🛜 [BLE] ❌ Error discovering characteristics: \(error.localizedDescription)")
            return
        }
        
        guard let characteristics = service.characteristics else {
            NSLog("🛜 [BLE] ⚠️ No characteristics found for service \(service.uuid)")
            return
        }
        
        NSLog("🛜 [BLE] 📋 Discovered \(characteristics.count) characteristic(s)")
        
        for characteristic in characteristics {
            NSLog("🛜 [BLE]    - Characteristic: \(characteristic.uuid), properties: \(characteristicPropertiesDescription(characteristic.properties))")
            
            switch characteristic.uuid {
            case MeshtasticBLE.toRadioUUID:
                NSLog("🛜 [BLE] ✅ Found TORADIO characteristic")
                toRadioCharacteristic = characteristic
                
            case MeshtasticBLE.fromRadioUUID:
                NSLog("🛜 [BLE] ✅ Found FROMRADIO characteristic")
                fromRadioCharacteristic = characteristic
                // FROMRADIO is read-only (no notify support) — we read it when fromNum notifies
                
            case MeshtasticBLE.fromNumUUID:
                NSLog("🛜 [BLE] ✅ Found FROMNUM (Notify) characteristic")
                fromNumCharacteristic = characteristic
                // Subscribe to notifications
                NSLog("🛜 [BLE] 📬 Subscribing to FROMNUM notifications...")
                peripheral.setNotifyValue(true, for: characteristic)
                
            case MeshtasticBLE.logRadioUUID:
                NSLog("🛜 [BLE] ✅ Found LOGRADIO (Notify) characteristic")
                logRadioCharacteristic = characteristic
                // Subscribe to notifications
                NSLog("🛜 [BLE] 📬 Subscribing to LOGRADIO notifications...")
                peripheral.setNotifyValue(true, for: characteristic)
                
            default:
                NSLog("🛜 [BLE] ℹ️ Found other characteristic: \(characteristic.uuid)")
                break
            }
        }
        
        // Only toRadio, fromRadio, and fromNum are required; logRadio is optional
        if toRadioCharacteristic != nil && fromRadioCharacteristic != nil && fromNumCharacteristic != nil {
            if logRadioCharacteristic == nil {
                NSLog("🛜 [BLE] ℹ️ LogRadio characteristic not found (optional, continuing)")
            }
            // Do NOT signal .connected yet — wait for the FROMNUM CCCD write to be
            // acknowledged by the peripheral (didUpdateNotificationStateFor).  If we
            // fire .connected now the WantConfig write races with the subscription ACK
            // and the device's FROMNUM notifications are silently dropped.
            NSLog("🛜 [BLE] ✅ Required characteristics found — awaiting FROMNUM subscription confirmation")
        } else {
            NSLog("🛜 [BLE] ⚠️ Missing required characteristics - toRadio: \(toRadioCharacteristic != nil), fromRadio: \(fromRadioCharacteristic != nil), fromNum: \(fromNumCharacteristic != nil)")
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            NSLog("🛜 [BLE] ❌ Notification subscription failed for \(characteristic.meshtasticCharacteristicName): \(error.localizedDescription)")
            return
        }
        let state = characteristic.isNotifying ? "enabled" : "disabled"
        NSLog("🛜 [BLE] 🔔 Notifications \(state) for \(characteristic.meshtasticCharacteristicName)")

        // Signal connected only once the FROMNUM subscription is confirmed.
        // At this point the peripheral will definitely deliver future FROMNUM
        // notifications, so it is safe to send WantConfig.
        if characteristic.uuid == MeshtasticBLE.fromNumUUID && characteristic.isNotifying {
            NSLog("🛜 [BLE] 🎉 FROMNUM subscription confirmed — Connection established!")
            connectionState.send(.connected)
            // Drain any packets the device may have already queued
            readFromRadioQueue()
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            NSLog("🛜 [BLE] ❌ Error updating value for characteristic: \(error.localizedDescription)")
            return
        }
        
        NSLog("🛜 [BLE] Did update value for \(characteristic.meshtasticCharacteristicName)")
        
        guard let data = characteristic.value else { return }
        
        switch characteristic.uuid {
        case MeshtasticBLE.fromRadioUUID:
            // Mark drain as complete before potentially kicking off the next read.
            isDrainingFromRadio = false
            guard !data.isEmpty else {
                NSLog("🛜 [BLE] 📭 FromRadio queue drained (empty response)")
                return
            }
            NSLog("🛜 [BLE] 📨 FromRadio: forwarding \(data.count) bytes to client")
            receivedDataSubject.send(data)
            // Immediately read next packet — device queues multiple FromRadio messages
            readFromRadioQueue()

        case MeshtasticBLE.fromNumUUID:
            NSLog("🛜 [BLE] 🔔 FromNum notification — draining packet queue")
            readFromRadioQueue()
            
        case MeshtasticBLE.logRadioUUID:
            if let logRecord = try? LogRecord(serializedBytes: data) {
                NSLog("🛜 [BLE] Received Log: \(logRecord.message)")
            }
            
        default:
            break
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard !pendingSendContinuations.isEmpty else {
            NSLog("🛜 [BLE] ⚠️ Received write callback but no pending continuations")
            return
        }

        let continuation = pendingSendContinuations.removeFirst()
        if let error = error {
            NSLog("🛜 [BLE] ❌ Write failed: \(error.localizedDescription)")
            continuation.resume(throwing: error)
        } else {
            NSLog("🛜 [BLE] ✅ Write succeeded")
            continuation.resume()
            // Immediately kick off a drain attempt after every TORADIO write.
            // The device may have queued response packets before FROMNUM notification arrives.
            if characteristic.uuid == MeshtasticBLE.toRadioUUID {
                readFromRadioQueue()
            }
        }
    }
}

/// Returns a human-readable description for a CBManagerState value.
private func cbManagerStateDescription(_ state: CBManagerState) -> String {
    switch state {
    case .unknown: return "unknown"
    case .resetting: return "resetting"
    case .unsupported: return "unsupported"
    case .unauthorized: return "unauthorized"
    case .poweredOff: return "poweredOff"
    case .poweredOn: return "poweredOn"
    @unknown default: return "unhandled state"
    }
}

/// Returns a human-readable description for a CBPeripheralState value.
private func cbPeripheralStateDescription(_ state: CBPeripheralState) -> String {
    switch state {
    case .disconnected:
        return "disconnected"
    case .connecting:
        return "connecting"
    case .connected:
        return "connected"
    case .disconnecting:
        return "disconnecting"
    @unknown default:
        return "unhandled state"
    }
}
/// Returns a human-readable description for CBCharacteristicProperties.
private func characteristicPropertiesDescription(_ properties: CBCharacteristicProperties) -> String {
    var descriptions: [String] = []
    
    if properties.contains(.broadcast) { descriptions.append("broadcast") }
    if properties.contains(.read) { descriptions.append("read") }
    if properties.contains(.writeWithoutResponse) { descriptions.append("writeWithoutResponse") }
    if properties.contains(.write) { descriptions.append("write") }
    if properties.contains(.notify) { descriptions.append("notify") }
    if properties.contains(.indicate) { descriptions.append("indicate") }
    if properties.contains(.authenticatedSignedWrites) { descriptions.append("authenticatedSignedWrites") }
    if properties.contains(.extendedProperties) { descriptions.append("extendedProperties") }
    if properties.contains(.notifyEncryptionRequired) { descriptions.append("notifyEncryptionRequired") }
    if properties.contains(.indicateEncryptionRequired) { descriptions.append("indicateEncryptionRequired") }
    
    return descriptions.isEmpty ? "none" : descriptions.joined(separator: ", ")
}

extension CBCharacteristic {
    
    var meshtasticCharacteristicName: String {
        switch self.uuid {
        case MeshtasticBLE.toRadioUUID:
            return "TORADIO"
        case MeshtasticBLE.fromRadioUUID:
            return "FROMRADIO"
        case MeshtasticBLE.fromNumUUID:
            return "FROMNUM"
        case MeshtasticBLE.logRadioUUID:
            return "LOGRADIO"
        default:
            return "UNKNOWN (\(self.uuid.uuidString))"
        }
    }
}

