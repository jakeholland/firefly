//
//  BluetoothServiceProtocol.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// Protocol for Bluetooth transport layer
/// Manages CoreBluetooth connection to Meshtastic device
protocol BluetoothServiceProtocol {
    /// Current connection state
    var connectionState: CurrentValueSubject<BluetoothConnectionState, Never> { get }
    
    /// Publisher for received data packets from the device
    var receivedDataPublisher: AnyPublisher<Data, Never> { get }
    
    /// Publisher for discovered devices during scanning
    var discoveredDevicesPublisher: AnyPublisher<[PeripheralDevice], Never> { get }
    
    /// Currently discovered devices
    var discoveredDevices: [PeripheralDevice] { get }
    
    /// Start scanning for Meshtastic devices
    func startScanning()
    
    /// Stop scanning
    func stopScanning()
    
    /// Connect to a specific device
    func connect(to deviceIdentifier: UUID)
    
    /// Disconnect from current device
    func disconnect()
    
    /// Send data to the connected device
    func send(_ data: Data) async throws
}

enum BluetoothConnectionState: Equatable {
    case disconnected
    case scanning
    case connecting
    case connected
    case resetting
    case unknown
    case failed(Error)
    
    static func == (lhs: BluetoothConnectionState, rhs: BluetoothConnectionState) -> Bool {
        switch (lhs, rhs) {
        case (.disconnected, .disconnected),
             (.scanning, .scanning),
             (.connecting, .connecting),
             (.connected, .connected),
             (.resetting, .resetting),
             (.unknown, .unknown):
            return true
        case (.failed(let lhsError), .failed(let rhsError)):
            return lhsError.localizedDescription == rhsError.localizedDescription
        default:
            return false
        }
    }
}

enum BluetoothError: LocalizedError {
    case notConnected
    case sendFailed
    case deviceNotFound
    case unauthorized
    case unsupported
    
    var errorDescription: String? {
        switch self {
        case .notConnected:
            return "Bluetooth device is not connected"
        case .sendFailed:
            return "Failed to send data to device"
        case .deviceNotFound:
            return "Meshtastic device not found"
        case .unauthorized:
            return "Bluetooth is unauthorized on this device"
        case .unsupported:
            return "Bluetooth is unsupported on this device"
        }
    }
}
