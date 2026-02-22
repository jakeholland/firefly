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
    case failed(Error)
    
    static func == (lhs: BluetoothConnectionState, rhs: BluetoothConnectionState) -> Bool {
        switch (lhs, rhs) {
        case (.disconnected, .disconnected),
             (.scanning, .scanning),
             (.connecting, .connecting),
             (.connected, .connected):
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
    
    var errorDescription: String? {
        switch self {
        case .notConnected:
            return "Bluetooth device is not connected"
        case .sendFailed:
            return "Failed to send data to device"
        case .deviceNotFound:
            return "Meshtastic device not found"
        }
    }
}
