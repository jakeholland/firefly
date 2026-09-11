//
//  DemoWorld.swift — Firefly Fields, the fictional festival
//  `docs/specs/S20-demo-mode.md` invented for the puck, reused here for
//  the app: same venue, same "brand-safe and funnier" fictional-only
//  rule. The app's M1 screens (Connect/Radar/Inbox/Settings — A01's
//  scope, no Lineup/Map yet) don't need the lineup or the wall clock
//  S20 seeds for the puck's Now screen, so this world is narrower: just
//  the crew, their signals, and one thread's worth of chatter, enough
//  to light up every face M1 actually has.
//
//  Coordinates are the festpack's OWN
//  (`firmware/assets/demo/firefly-fields.festpack.json`, "venue" and
//  "map.features") — never a real place, and never the phone owner's
//  actual home (`docs/memory/firefly-touch-cal-default.md`'s honesty
//  rule, applied here too): the demo world is bench-like on purpose.
//
import FireflyMesh
import Foundation

/// Stable node numbers for the fictional crew — never real Meshtastic
/// `my_node_num`s (those come off a radio's own random 32 bits), just
/// small, obviously-fake constants a test or a screenshot script can
/// refer to by name instead of a magic number.
public enum DemoCrew {
    public static let jake: UInt32 = 0x0000_1001 // "my" node — the connected radio
    public static let taylor: UInt32 = 0x0000_1002
    public static let dana: UInt32 = 0x0000_1003
    public static let sam: UInt32 = 0x0000_1004
    public static let mo: UInt32 = 0x0000_1005 // paired, never heard — the honest LOST case
    public static let camp: UInt32 = 0x0000_1006 // an asserted landmark, not a person
    public static let stranger: UInt32 = 0x0000_1007 // heard, never paired
}

public struct DemoWorld: Sendable {
    /// Firefly Fields' own map anchor — `firefly-fields.festpack.json`,
    /// "venue" (also "The Firefly Tower" landmark: the two coincide).
    /// This is also the phone's own demo fix (`phoneFix`, below).
    public static let venueLatitude = 43.700000
    public static let venueLongitude = -121.500000

    /// CAMP's own asserted position — the festpack's "Main Gate"
    /// coordinate (`firefly-fields.festpack.json`, "map.features"),
    /// ~55 m from `venueLatitude`/`venueLongitude`: far enough that
    /// `ff_crew_close_range`'s 30 m distance leg cannot fire for it
    /// (`DemoRunner`'s own comment on this constant's use).
    public static let campLatitude = 43.699506
    public static let campLongitude = -121.500000

    /// M2's inbound-RALLY screenshot (`-FireflyDemoScreen rally`): a
    /// fictional meeting spot near The Beacon, close enough to the
    /// phone's own demo fix (`phoneFix`, below) that the rendered
    /// distance/bearing reads as a plausible short walk rather than a
    /// cross-venue trek.
    public static let rallyLatitude = 43.700450
    public static let rallyLongitude = -121.499600
    public static let rallyName = "THE TOWER"

    public let myNodeNum: UInt32
    /// The scripted `nodeDB` dump `DemoMeshtasticClient.connect()`
    /// plays — Taylor, Dana, Sam, and the heard-only stranger. Mo and
    /// CAMP are deliberately NOT in here (see `DemoRunner`'s own
    /// comment): Mo must never be `has_heard` for RADAR_LOST to be
    /// honest, and CAMP's asserted position is seeded the same way a
    /// real "somebody typed this in" landmark would be, straight onto
    /// `ff_crew`, not off a radio packet.
    public let nodeDB: [MeshNodeSnapshot]
    public let incomingFromTaylor: IncomingText
    public let phoneFix: LocationFix
    public let phoneHeading: HeadingReading
    /// A second, later RSSI-only reading for Taylor (same identity,
    /// no position change) — `CoreStore.apply(nodeUpdate:)` feeds
    /// `ff_crew_on_rssi` again, giving the no-GPS signal view an actual
    /// RISING trend to show instead of a flat first sample.
    public let taylorSecondRSSI: MeshNodeSnapshot

    public static func fireflyFields(now: Date = Date()) -> DemoWorld {
        let my = DemoCrew.jake

        // ~142 m bearing 045° (NE) from the venue anchor — S20's own
        // "DANA... ~30 m (fresh arrow + distance)" pattern, just a
        // different distance/member so this world's LIVE member reads
        // clearly on a compass ring rather than close-range.
        //
        // RSSI deliberately kept AT OR WEAKER than -60 dBm
        // (`FF_CREW_CLOSE_RANGE_DBM`, `ff_crew.h`): `ff_crew_close_range`
        // has a SECOND leg — a strong, recent RSSI alone counts as close
        // range even at real GPS distance — and a −58 dBm sample here
        // silently forced RADAR_CLOSE instead of RADAR_LIVE/RADAR_SIGNAL
        // (caught by `DemoRunnerTests`, not a reading — see that leg's
        // own doc comment in `ff_crew.h`).
        let taylor = MeshNodeSnapshot(
            num: DemoCrew.taylor, shortName: "TAY", longName: "Taylor",
            position: NodePosition(latitude: 43.700902, longitude: -121.498753, time: now,
                                    source: .internalGPS, precisionBits: nil),
            lastHeard: now, rssiDbm: -72, snrDb: 4.0, hopsAway: 0)

        // Near The Beacon (the festpack's own mainstage coordinate) —
        // live, a shorter hop than Taylor's.
        let dana = MeshNodeSnapshot(
            num: DemoCrew.dana, shortName: "DANA", longName: "Dana",
            position: NodePosition(latitude: 43.700269, longitude: -121.500000, time: now,
                                    source: .internalGPS, precisionBits: nil),
            lastHeard: now, rssiDbm: -66, snrDb: 4.0, hopsAway: 0)

        // Inside Camp Glow, but the fix itself is 6 minutes old —
        // FF_CREW_LIVE_MS is 45 s (ff_crew.h), so this lands squarely
        // in STALE ("LAST SEEN 6 MIN") without touching FF_CREW_LOST_MS
        // (20 min).
        let samTime = now.addingTimeInterval(-6 * 60)
        let sam = MeshNodeSnapshot(
            num: DemoCrew.sam, shortName: "SAM", longName: "Sam",
            position: NodePosition(latitude: 43.700808, longitude: -121.501118, time: samTime,
                                    source: .internalGPS, precisionBits: nil),
            lastHeard: samTime, rssiDbm: -89, snrDb: -2.0, hopsAway: 0)

        // Heard (so it lists on Connect's Nearby section and in the
        // NodeDB) but never `setPaired` — S20's "one heard-only
        // stranger" (its own crew seed list), here a node with no
        // identity at all, which is the honest default for someone the
        // radio has heard packets from but who was never named
        // (`CoreStore.apply(nodeUpdate:)`'s own "nothing is
        // synthesized" rule).
        let stranger = MeshNodeSnapshot(
            num: DemoCrew.stranger, shortName: nil, longName: nil, position: nil,
            lastHeard: now, rssiDbm: -102, snrDb: -6.5, hopsAway: 0)

        // A stronger but still not-close-range reading (this file's own
        // comment on `taylor`'s RSSI) — enough of a jump for
        // `ff_crew_rssi_trend` to read RISING. `lastHeard: now`,
        // deliberately NOT a few seconds into the future: `DemoRunner`
        // injects this a short delay after `now` was captured, and
        // `ff_crew`'s heard-age is `now_ms - last_heard_ms` in
        // UNSIGNED 32-bit arithmetic (`ff_radar.c`) — a `last_heard_ms`
        // that is still ahead of the real clock when this is processed
        // wraps that subtraction to ~49.7 days instead of going
        // negative, which is exactly the "heard 1193 HR ago" bug this
        // comment is here to keep from coming back.
        let taylorSecondRSSI = MeshNodeSnapshot(
            num: DemoCrew.taylor, shortName: "TAY", longName: "Taylor",
            position: taylor.position, lastHeard: now, rssiDbm: -61, snrDb: 8.5, hopsAway: 0)

        let incoming = IncomingText(
            from: DemoCrew.taylor, to: my, channel: 0, packetID: 777_001,
            text: "made it to the tower \u{2014} come find me!", rxTime: now, rssiDbm: -58, snrDb: 7.25,
            direct: true)

        let phoneFix = LocationFix(
            latitude: venueLatitude, longitude: venueLongitude, altitude: nil, time: now,
            horizontalAccuracyMeters: 6, groundSpeedMetersPerSecond: nil, groundTrackDegrees: nil)
        // Roughly facing Taylor's bearing (NE) so the LIVE arrow reads
        // as pointing somewhere plausible rather than straight down.
        let phoneHeading = HeadingReading(headingDegrees: 45, accuracyDegrees: 5)

        return DemoWorld(
            myNodeNum: my,
            nodeDB: [taylor, dana, sam, stranger],
            incomingFromTaylor: incoming,
            phoneFix: phoneFix,
            phoneHeading: phoneHeading,
            taylorSecondRSSI: taylorSecondRSSI)
    }
}
