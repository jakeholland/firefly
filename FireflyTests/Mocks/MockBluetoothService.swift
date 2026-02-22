//
//  MockBluetoothService.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import Combine

/// Mock Bluetooth service for testing
final class MockBluetoothService: BluetoothServiceProtocol {
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
    
    var didCallStartScanning = false
    var didCallStopScanning = false
    var didCallConnect = false
    var didCallDisconnect = false
    var sentData: [Data] = []
    
    init(initialState: BluetoothConnectionState = .disconnected) {
        self.connectionState = CurrentValueSubject(initialState)
    }
    
    func startScanning() {
        didCallStartScanning = true
        connectionState.send(.scanning)
    }
    
    func stopScanning() {
        didCallStopScanning = true
    }
    
    func connect(to deviceIdentifier: UUID) {
        didCallConnect = true
        connectionState.send(.connecting)
    }
    
    func disconnect() {
        didCallDisconnect = true
        connectionState.send(.disconnected)
    }
    
    func send(_ data: Data) async throws {
        sentData.append(data)
    }
    
    // Test helpers
    func simulateReceivedData(_ data: Data) {
        receivedDataSubject.send(data)
    }
    
    func simulateConnection() {
        connectionState.send(.connected)
    }
    
    func simulateDisconnection() {
        connectionState.send(.disconnected)
    }
    
    func simulateError(_ error: Error) {
        connectionState.send(.failed(error))
    }
    
    func simulateDiscoveredDevices(_ devices: [PeripheralDevice]) {
        discoveredDevicesSubject.send(devices)
    }
}
