//
//  MeshtasticBLEConstants.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import Foundation
import CoreBluetooth

/// Bluetooth UUIDs and constants for Meshtastic devices
/// Based on: https://github.com/meshtastic/Meshtastic-Apple
enum MeshtasticBLE {
    /// Meshtastic service UUID
    static let serviceUUID = CBUUID(string: "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD")
    
    /// ToRadio characteristic (app -> device)
    static let toRadioUUID = CBUUID(string: "F75C76D2-129E-4DAD-A1DD-7866124401E7")
    
    /// FromRadio characteristic (device -> app)
    static let fromRadioUUID = CBUUID(string: "8BA2BCC2-EE02-4A55-A531-C525C5E454D5")
    
    /// FromNum characteristic (number of queued messages)
    static let fromNumUUID = CBUUID(string: "ED9DA18C-A800-4F66-A670-AA7547E34453")
    
    /// Service name filter for scanning
    static let deviceNamePrefix = "Meshtastic"
}
