//
//  MapBridge.swift — the Swift-safe wrapper over `firmware/core/ff_map`
//  (docs/specs/S09-map-face.md) — the SAME fixed-fit camera transform,
//  local-flat projection, circle-clip and render-kind policy the puck's
//  own `scr_map.c` uses. `firmware/core/ff_map.c`/`.h` are already
//  symlinked into the `FireflyCore` SwiftPM target (same farm
//  `ff_crew`/`ff_geo`/`ff_radar` go through — `app/tools/
//  link_core_sources.sh`), so the app's schematic Field map projects
//  Firefly Fields through the IDENTICAL object code the puck's glass
//  does, rather than a from-scratch reimplementation that could quietly
//  disagree with it. `ff_geo_project` (core/ff_geo) supplies the
//  lat/lon -> local-flat east/north half of the pipeline S05 describes;
//  this file adds the east/north -> screen-px half.
//
//  Threading: stateless, same posture as `GeoBridge.swift` — every
//  function is a pure value-in/value-out call.
//
import FireflyCore
import Foundation

/// One point in the local flat (equirectangular) projection `ff_geo_project`
/// produces — meters east/north of whatever origin the caller chose.
public struct MapEastNorth: Sendable, Equatable {
    public let eastM: Float
    public let northM: Float
    public init(eastM: Float, northM: Float) {
        self.eastM = eastM
        self.northM = northM
    }
}

/// `ff_map_xform_t`, decoded — the fitted camera (uniform meters->px
/// scale + the meters-space point that maps to the circle's own center).
public struct MapCameraFit: Sendable, Equatable {
    public let scalePxPerM: Float
    public let centerEastM: Float
    public let centerNorthM: Float

    public init(scalePxPerM: Float, centerEastM: Float, centerNorthM: Float) {
        self.scalePxPerM = scalePxPerM
        self.centerEastM = centerEastM
        self.centerNorthM = centerNorthM
    }
}

/// `ff_map_render_kind_t` — see that C enum's own doc comment
/// (`ff_map.h`) for the full untraced-feature render policy this
/// mirrors 1:1. `.omit`/`.labelOnly`/`.stageStub`/`.line`/`.polygon`,
/// same names, same meaning — this is a decode, not a reinterpretation.
public enum MapFeatureRenderKind: Sendable, Equatable {
    case omit, labelOnly, stageStub, line, polygon

    init(ffKind: ff_map_render_kind_t) {
        switch ffKind {
        case FF_MAP_RENDER_OMIT: self = .omit
        case FF_MAP_RENDER_LABEL_ONLY: self = .labelOnly
        case FF_MAP_RENDER_STAGE_STUB: self = .stageStub
        case FF_MAP_RENDER_LINE: self = .line
        default: self = .polygon
        }
    }
}

public enum MapBridge {
    /// `ff_geo_project` — `p` relative to `origin`, local flat
    /// (equirectangular) meters. Accurate for festival-scale extents
    /// (<10 km, per that function's own doc comment); never used for
    /// anything larger.
    public static func project(_ point: GeoCoordinate, origin: GeoCoordinate) -> MapEastNorth {
        var east: Float = 0, north: Float = 0
        ff_geo_project(origin.ffValue, point.ffValue, &east, &north)
        return MapEastNorth(eastM: east, northM: north)
    }

    /// `ff_map_xform_fit` — the S09 "fixed-fit v1" camera: bounding box
    /// of `points` into a circle of `radiusPx` with `marginPx` clearance,
    /// north-up, aspect preserved. Empty `points` falls back to the
    /// spec's own "1 km square around origin" (see `ff_map.h`'s doc
    /// comment) — never a divide-by-zero. Builds one flat, tightly
    /// packed `[Float]` buffer matching C's `float pts_en[][2]` layout
    /// byte-for-byte (Swift's `[[Float]]` is NOT laid out that way —
    /// it's an array of independently heap-allocated arrays).
    public static func fit(points: [MapEastNorth], radiusPx: Float, marginPx: Float) -> MapCameraFit {
        var out = ff_map_xform_t()
        if points.isEmpty {
            ff_map_xform_fit(&out, nil, 0, radiusPx, marginPx)
        } else {
            var flat = [Float](repeating: 0, count: points.count * 2)
            for (i, p) in points.enumerated() {
                flat[i * 2] = p.eastM
                flat[i * 2 + 1] = p.northM
            }
            flat.withUnsafeBufferPointer { buf in
                buf.baseAddress!.withMemoryRebound(to: (Float, Float).self, capacity: points.count) { pairs in
                    ff_map_xform_fit(&out, pairs, Int32(points.count), radiusPx, marginPx)
                }
            }
        }
        return MapCameraFit(scalePxPerM: out.scale_px_per_m, centerEastM: out.center_east_m,
                             centerNorthM: out.center_north_m)
    }

    public static func projectToScreen(_ point: MapEastNorth, camera: MapCameraFit) -> (x: Float, y: Float) {
        var x: Float = 0, y: Float = 0
        var xform = ff_map_xform_t(scale_px_per_m: camera.scalePxPerM, center_east_m: camera.centerEastM,
                                    center_north_m: camera.centerNorthM)
        ff_map_project(&xform, point.eastM, point.northM, &x, &y)
        return (x, y)
    }

    public static func clipToCircle(x: Float, y: Float, radiusPx: Float) -> (x: Float, y: Float) {
        var outX: Float = 0, outY: Float = 0
        ff_map_clip_point_to_circle(x, y, radiusPx, &outX, &outY)
        return (outX, outY)
    }

    /// `ff_map_feature_render_kind` — see `MapFeatureRenderKind`'s doc
    /// comment.
    public static func renderKind(pointCount: Int, isStage: Bool) -> MapFeatureRenderKind {
        MapFeatureRenderKind(ffKind: ff_map_feature_render_kind(UInt8(clamping: pointCount), isStage ? 1 : 0))
    }
}
