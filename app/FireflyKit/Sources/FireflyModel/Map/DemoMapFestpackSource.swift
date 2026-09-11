//
//  DemoMapFestpackSource.swift — Map tab slice: a `MapFestpackSource`
//  carrying Firefly Fields, transcribed verbatim from the SAME pack the
//  rest of this app's demo mode already uses
//  (`firmware/assets/demo/firefly-fields.festpack.json` — see
//  `DemoWorld.swift`'s own header comment: "never a real place, and
//  never the phone owner's actual home"). Coordinates, stage colors,
//  and feature kinds/labels below are copied from that JSON file, not
//  invented — the venue anchor (43.7000, -121.5000) is byte-identical
//  to `DemoWorld.venueLatitude`/`venueLongitude`, so the Map tab's
//  demo world and the rest of the app's demo world are the same
//  festival, not two different ones that happen to share a name.
//
//  This is the seam a real `FestpackProviding`-backed adapter swaps in
//  for (see `Festpack.swift`'s header comment) — nothing outside this
//  file knows or cares that the data is hardcoded here.
//
import Foundation

public final class DemoMapFestpackSource: MapFestpackSource {
    public init() {}

    public func currentFestpack() async -> Festpack? {
        DemoMapFestpackSource.fireflyFields
    }

    /// Firefly Fields, Firefly's own fictional demo festival — see this
    /// file's header comment for provenance. `stages[].colorHex` are the
    /// pack's own hex strings ("#ffc66b" etc.), parsed once here rather
    /// than at every render.
    public static let fireflyFields: Festpack = {
        let venue = FestpackLatLon(latitude: 43.7000, longitude: -121.5000)

        let stages: [FestpackStage] = [
            FestpackStage(id: "beacon", name: "The Beacon", colorHex: 0xFFC66B,
                          polygon: [], centre: FestpackLatLon(latitude: 43.700269, longitude: -121.500000)),
            FestpackStage(id: "hollow", name: "Bass Hollow", colorHex: 0xB08CFF,
                          polygon: [], centre: FestpackLatLon(latitude: 43.699281, longitude: -121.498882)),
            FestpackStage(id: "grove", name: "Sunrise Grove", colorHex: 0x4FD8C4,
                          polygon: [], centre: FestpackLatLon(latitude: 43.700090, longitude: -121.498758)),
            FestpackStage(id: "lantern", name: "The Lantern", colorHex: 0xFF5CA8,
                          polygon: [], centre: FestpackLatLon(latitude: 43.700090, longitude: -121.501305)),
            FestpackStage(id: "glowworm", name: "Glowworm", colorHex: 0x9BE07B,
                          polygon: [], centre: FestpackLatLon(latitude: 43.699461, longitude: -121.500870)),
        ]

        // "map.features" from firefly-fields.festpack.json, transcribed
        // verbatim (kind/label/polygon). Untraced stage points render as
        // the S09 labeled-stub circle; the two `camping` features are
        // the pack's own 4-point rectangles (a real `FF_MAP_RENDER_POLYGON`
        // case); every other feature is a single point (`FF_MAP_RENDER_
        // LABEL_ONLY` — no invented shape, per `ff_map_feature_render_kind`).
        let features: [FestpackFeature] = [
            FestpackFeature(id: "stage-beacon", kind: .stage, label: "The Beacon", stageID: "beacon",
                             polygon: [FestpackLatLon(latitude: 43.700269, longitude: -121.500000)]),
            FestpackFeature(id: "stage-hollow", kind: .stage, label: "Bass Hollow", stageID: "hollow",
                             polygon: [FestpackLatLon(latitude: 43.699281, longitude: -121.498882)]),
            FestpackFeature(id: "stage-grove", kind: .stage, label: "Sunrise Grove", stageID: "grove",
                             polygon: [FestpackLatLon(latitude: 43.700090, longitude: -121.498758)]),
            FestpackFeature(id: "stage-lantern", kind: .stage, label: "The Lantern", stageID: "lantern",
                             polygon: [FestpackLatLon(latitude: 43.700090, longitude: -121.501305)]),
            FestpackFeature(id: "stage-glowworm", kind: .stage, label: "Glowworm", stageID: "glowworm",
                             polygon: [FestpackLatLon(latitude: 43.699461, longitude: -121.500870)]),
            FestpackFeature(id: "firefly-tower", kind: .poi, label: "The Firefly Tower",
                             polygon: [FestpackLatLon(latitude: 43.700000, longitude: -121.500000)]),
            FestpackFeature(id: "main-gate", kind: .entrance, label: "Main Gate",
                             polygon: [FestpackLatLon(latitude: 43.699506, longitude: -121.500000)]),
            FestpackFeature(id: "medical", kind: .medical, label: "Medical",
                             polygon: [FestpackLatLon(latitude: 43.699641, longitude: -121.500497)]),
            FestpackFeature(id: "water-1", kind: .water, label: "Water Refill",
                             polygon: [FestpackLatLon(latitude: 43.700359, longitude: -121.499689)]),
            FestpackFeature(id: "water-2", kind: .water, label: "Water Refill",
                             polygon: [FestpackLatLon(latitude: 43.699551, longitude: -121.499751)]),
            FestpackFeature(id: "food-row", kind: .vendor, label: "Food Row",
                             polygon: [FestpackLatLon(latitude: 43.700314, longitude: -121.500559)]),
            FestpackFeature(id: "silent-disco", kind: .poi, label: "Silent Disco",
                             polygon: [FestpackLatLon(latitude: 43.699731, longitude: -121.499254)]),
            FestpackFeature(id: "art-car", kind: .poi, label: "The Art Car",
                             polygon: [FestpackLatLon(latitude: 43.700449, longitude: -121.499627)]),
            FestpackFeature(id: "ferris-wheel", kind: .poi, label: "Ferris Wheel",
                             polygon: [FestpackLatLon(latitude: 43.700108, longitude: -121.500124)]),
            FestpackFeature(id: "camp-glow", kind: .camping, label: "Camp Glow", polygon: [
                FestpackLatLon(latitude: 43.700988, longitude: -121.501368),
                FestpackLatLon(latitude: 43.700988, longitude: -121.500868),
                FestpackLatLon(latitude: 43.700628, longitude: -121.500868),
                FestpackLatLon(latitude: 43.700628, longitude: -121.501368),
            ]),
            FestpackFeature(id: "camp-ember", kind: .camping, label: "Camp Ember", polygon: [
                FestpackLatLon(latitude: 43.699372, longitude: -121.499132),
                FestpackLatLon(latitude: 43.699372, longitude: -121.498632),
                FestpackLatLon(latitude: 43.699012, longitude: -121.498632),
                FestpackLatLon(latitude: 43.699012, longitude: -121.499132),
            ]),
        ]

        // A short slice of "schedule" — enough for a Map tab that wants
        // to show "on now at this stage" later; not required by any
        // acceptance criterion this slice ships, carried through because
        // `Festpack`'s own shape names it.
        let schedule: [FestpackScheduleItem] = [
            FestpackScheduleItem(artist: "FIREFLY", stageID: "beacon", day: "2026-09-05", start: "21:00",
                                  end: "22:30", note: "Sat headliner"),
            FestpackScheduleItem(artist: "BASS FAUNA", stageID: "hollow", day: "2026-09-04", start: "21:00",
                                  end: "22:00"),
        ]

        return Festpack(meta: FestpackMeta(name: "Firefly Fields", venue: venue), stages: stages,
                         features: features, schedule: schedule)
    }()
}
