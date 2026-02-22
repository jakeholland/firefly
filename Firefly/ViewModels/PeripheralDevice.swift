//
//  PeripheralDevice.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import CoreBluetooth

/// Represents a discovered Bluetooth peripheral device
struct PeripheralDevice: Identifiable, Equatable {
    let id: UUID
    let name: String
    let rssi: Int
    let peripheral: CBPeripheral
    
    init(peripheral: CBPeripheral, rssi: Int) {
        self.id = peripheral.identifier
        self.name = peripheral.name ?? "Unknown Device"
        self.rssi = rssi
        self.peripheral = peripheral
    }
    
    var signalStrength: String {
        if rssi > -50 {
            return "Excellent"
        } else if rssi > -70 {
            return "Good"
        } else if rssi > -85 {
            return "Fair"
        } else {
            return "Weak"
        }
    }
    
    static func == (lhs: PeripheralDevice, rhs: PeripheralDevice) -> Bool {
        lhs.id == rhs.id
    }
}
