//
//  CoreBluetoothService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import CoreBluetooth
import Combine

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
    
    private var pendingSendContinuations: [CheckedContinuation<Void, Error>] = []
    
    // MARK: - Initialization
    
    override init() {
        self.connectionState = CurrentValueSubject(.disconnected)
        super.init()
        
        // Initialize central manager on main queue
        self.centralManager = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [CBCentralManagerOptionShowPowerAlertKey: true]
        )
    }
    
    // MARK: - Public Methods
    
    func startScanning() {
        guard centralManager.state == .poweredOn else {
            connectionState.send(.failed(BluetoothError.deviceNotFound))
            return
        }
        
        connectionState.send(.scanning)
        centralManager.scanForPeripherals(
            withServices: [MeshtasticBLE.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }
    
    func stopScanning() {
        centralManager.stopScan()
        discoveredDevicesSubject.send([])
    }
    
    func connect(to deviceIdentifier: UUID) {
        stopScanning()
        
        // Find the peripheral from discovered devices or retrieve from system
        let peripheral: CBPeripheral?
        
        if let device = discoveredDevicesSubject.value.first(where: { $0.id == deviceIdentifier }) {
            peripheral = device.peripheral
        } else {
            peripheral = centralManager.retrievePeripherals(withIdentifiers: [deviceIdentifier]).first
        }
        
        guard let peripheral = peripheral else {
            connectionState.send(.failed(BluetoothError.deviceNotFound))
            return
        }
        
        connectedPeripheral = peripheral
        peripheral.delegate = self
        connectionState.send(.connecting)
        centralManager.connect(peripheral, options: nil)
    }
    
    func disconnect() {
        if let peripheral = connectedPeripheral {
            centralManager.cancelPeripheralConnection(peripheral)
        }
        cleanup()
    }
    
    func send(_ data: Data) async throws {
        guard let characteristic = toRadioCharacteristic,
              let peripheral = connectedPeripheral else {
            throw BluetoothError.notConnected
        }
        
        return try await withCheckedThrowingContinuation { continuation in
            pendingSendContinuations.append(continuation)
            peripheral.writeValue(data, for: characteristic, type: .withResponse)
        }
    }
    
    // MARK: - Private Methods
    
    private func cleanup() {
        toRadioCharacteristic = nil
        fromRadioCharacteristic = nil
        connectedPeripheral = nil
        discoveredDevicesSubject.send([])
        connectionState.send(.disconnected)
    }
}

// MARK: - CBCentralManagerDelegate

extension CoreBluetoothService: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            // Ready to scan
            break
        case .poweredOff, .unsupported, .unauthorized:
            connectionState.send(.disconnected)
        default:
            break
        }
    }
    
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        // Add to discovered devices list
        let device = PeripheralDevice(peripheral: peripheral, rssi: RSSI.intValue)
        var devices = discoveredDevicesSubject.value
        
        // Update existing device or add new one
        if let index = devices.firstIndex(where: { $0.id == device.id }) {
            devices[index] = device
        } else {
            devices.append(device)
        }
        
        discoveredDevicesSubject.send(devices)
    }
    
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([MeshtasticBLE.serviceUUID])
    }
    
    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        connectionState.send(.failed(error ?? BluetoothError.deviceNotFound))
        cleanup()
    }
    
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        cleanup()
    }
}

// MARK: - CBPeripheralDelegate

extension CoreBluetoothService: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == MeshtasticBLE.serviceUUID }) else {
            connectionState.send(.failed(BluetoothError.deviceNotFound))
            return
        }
        
        peripheral.discoverCharacteristics(
            [MeshtasticBLE.toRadioUUID, MeshtasticBLE.fromRadioUUID],
            for: service
        )
    }
    
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }
        
        for characteristic in characteristics {
            switch characteristic.uuid {
            case MeshtasticBLE.toRadioUUID:
                toRadioCharacteristic = characteristic
                
            case MeshtasticBLE.fromRadioUUID:
                fromRadioCharacteristic = characteristic
                // Subscribe to notifications
                peripheral.setNotifyValue(true, for: characteristic)
                
            default:
                break
            }
        }
        
        // Once both characteristics are discovered, we're connected
        if toRadioCharacteristic != nil && fromRadioCharacteristic != nil {
            connectionState.send(.connected)
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == MeshtasticBLE.fromRadioUUID,
              let data = characteristic.value else {
            return
        }
        
        // Publish received data
        receivedDataSubject.send(data)
    }
    
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard !pendingSendContinuations.isEmpty else { return }
        
        let continuation = pendingSendContinuations.removeFirst()
        if let error = error {
            continuation.resume(throwing: error)
        } else {
            continuation.resume()
        }
    }
}
