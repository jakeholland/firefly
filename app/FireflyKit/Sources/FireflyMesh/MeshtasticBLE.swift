//
//  MeshtasticBLE.swift — the Meshtastic BLE GATT vocabulary.
//
//  ATTRIBUTION: these five UUIDs and the roles below are Meshtastic's
//  published client GATT contract. They were cross-checked against two
//  independent sources rather than typed from memory:
//    - Meshtastic-Apple (GPL-3.0), the official iOS/macOS client;
//    - this repo's own archived iOS app at commit 8b0967f,
//      `Firefly/Core/Models/MeshtasticBLEConstants.swift`.
//  Firefly is GPL-3.0 too, so borrowing from Meshtastic-Apple is
//  license-compatible; see docs/LICENSING.md and
//  docs/specs/A01-companion-app.md's "Reuse assessment".
//
import Foundation

public enum MeshtasticBLE {
    /// The Meshtastic service every node advertises. Scanning filters on
    /// this, never on the "Meshtastic" name prefix: a renamed node still
    /// advertises the service, and a name prefix is trivially spoofable.
    public static let serviceUUIDString = "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD"

    /// Write-only. A client writes one length-prefix-free `ToRadio`
    /// protobuf per write.
    public static let toRadioUUIDString = "F75C76D2-129E-4DAD-A1DD-7866124401E7"

    /// Read-only, **not** notifying. Each read returns exactly one queued
    /// `FromRadio` protobuf; an EMPTY read means the queue is drained.
    /// That empty-read terminator is the whole loop contract — see
    /// `FromRadioDrainPolicy`.
    public static let fromRadioUUIDString = "2C55E69E-4993-11ED-B878-0242AC120002"

    /// Notifying. Carries a packet counter, not data: it is the nudge
    /// that says "there is something to read from FROMRADIO now".
    public static let fromNumUUIDString = "ED9DA18C-A800-4F66-A670-AA7547E34453"

    /// Optional. Absent on older firmware — a missing LOGRADIO is not a
    /// connection failure.
    public static let logRadioUUIDString = "5A3D6E49-06E6-4423-9944-E9DE8CDF9547"
}

/// When a client may consider itself connected, and when it may stop
/// reading FROMRADIO.
///
/// Both rules are borrowed behaviour, not invention:
///
/// 1. **Do not send `want_config` until the FROMNUM subscription is
///    ACKed.** The archived app (8b0967f,
///    `Firefly/Services/CoreBluetoothService.swift`) defers its
///    `.connected` transition from `didDiscoverCharacteristicsFor` to
///    `didUpdateNotificationStateFor` for exactly this reason, with the
///    comment "the WantConfig write races with the subscription ACK and
///    the device's FROMNUM notifications are silently dropped". It
///    presents as "connects fine, then receives nothing, sometimes" —
///    expensive to re-derive, so it is pinned here as a named policy and
///    asserted by a test rather than left as a comment in a delegate.
///
/// 2. **An empty FROMRADIO read is the end of the queue.** Drain by
///    re-reading until empty; re-kick the drain on (a) subscription ACK,
///    (b) every FROMNUM notification, and (c) after every successful
///    TORADIO write, because the radio may have queued a reply before
///    the notification lands.
public enum FromRadioDrainPolicy {
    /// Reasons a drain is (re)started. Kept as a type so the transport
    /// can be tested for "did it re-kick after a write" without a radio.
    public enum Trigger: String, Sendable, CaseIterable {
        case subscriptionAcknowledged
        case fromNumNotification
        case toRadioWriteCompleted
    }

    /// `true` when a read of FROMRADIO means "nothing left".
    public static func isQueueDrained(read data: Data) -> Bool { data.isEmpty }
}
