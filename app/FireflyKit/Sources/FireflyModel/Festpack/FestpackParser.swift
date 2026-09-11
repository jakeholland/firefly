//
//  FestpackParser.swift — the Swift-safe wrapper over `fp_parse`
//  (docs/specs/S05-festpack.md; firmware/festpack/src/fp_pack.c).
//
//  THE ONE parser: the coordinator's explicit instruction was never to
//  write a second JSON schema parser in Swift, so this file's only job
//  is turning a parsed `fp_pack_t` into a `Festpack` — it has no schema
//  knowledge of its own, and it never touches the JSON text itself
//  beyond handing it to `fp_parse`.
//
//  "C types never leave the bridge" (Bridge/* convention, see
//  InboxBridge.swift's header comment): `fp_pack_t`/`jsmntok_t` live
//  entirely inside `parse(_:)`'s stack/heap scratch and are fully
//  decoded into plain `Festpack` values before returning.
//
import FireflyCore
import Foundation

public enum FestpackParseError: Sendable, Equatable, Error {
    case malformedJSON
    case wrongVersion
    /// An array exceeded its `FP_MAX_*` cap, or the input/token budget
    /// (`fp_pack.h`'s `FP_MAX_JSON_LEN`/`FP_MAX_TOKENS`) was exceeded.
    case tooBig

    init?(_ result: fp_result_t) {
        switch result {
        case FP_OK: return nil
        case FP_ERR_JSON: self = .malformedJSON
        case FP_ERR_VERSION: self = .wrongVersion
        case FP_ERR_TOO_BIG: self = .tooBig
        default: self = .malformedJSON
        }
    }
}

public enum FestpackParser {
    /// Parses raw festpack.json bytes into a `Festpack` through
    /// `fp_parse`. Allocates its own jsmn token scratch per call (heap,
    /// `FP_MAX_TOKENS` capacity, freed when this call returns) — unlike
    /// the ESP32-S3 device (S26 slice a), the phone has no internal-RAM-
    /// vs-PSRAM budget to economize for by keeping this scratch around
    /// between calls, and a pack is parsed at most once per fetch/cache
    /// load, never in a hot loop.
    public static func parse(_ json: Data) -> Result<Festpack, FestpackParseError> {
        var pack = fp_pack_t()
        let toks = UnsafeMutablePointer<jsmntok_t>.allocate(capacity: Int(FP_MAX_TOKENS))
        defer { toks.deallocate() }

        let result: fp_result_t = json.withUnsafeBytes { raw -> fp_result_t in
            guard let base = raw.bindMemory(to: CChar.self).baseAddress else { return FP_ERR_JSON }
            return fp_parse(base, raw.count, &pack, toks, Int32(FP_MAX_TOKENS))
        }

        if let error = FestpackParseError(result) { return .failure(error) }
        return .success(decode(pack))
    }

    // MARK: - fp_pack_t -> Festpack

    static func decode(_ pack: fp_pack_t) -> Festpack {
        let decodedStages = decodeStages(pack)
        let stageIDs = decodedStages.map(\.id)
        return Festpack(
            name: FixedCString.decode(pack.name),
            year: Int(pack.year),
            startDayOfYear: Int(pack.start_doy),
            endDayOfYear: Int(pack.end_doy),
            utcOffsetMinutes: Int(pack.utc_offset_min),
            utcOffsetAssumed: pack.utc_offset_assumed,
            originKnown: pack.origin_known,
            originApproximate: pack.origin_approx,
            stages: decodedStages,
            sets: decodeSets(pack, stageIDs: stageIDs),
            features: decodeFeatures(pack, stageIDs: stageIDs),
            landmarks: decodeLandmarks(pack),
            meta: decodeMeta(pack))
    }

    private static func decodeStages(_ pack: fp_pack_t) -> [FestpackStage] {
        withUnsafeBytes(of: pack.stages) { raw in
            let items = raw.bindMemory(to: fp_stage_t.self)
            return (0..<Int(pack.n_stages)).map { i in
                let s = items[i]
                return FestpackStage(id: FixedCString.decode(s.id), name: FixedCString.decode(s.name), colorRGB: s.color_rgb)
            }
        }
    }

    private static func decodeSets(_ pack: fp_pack_t, stageIDs: [String]) -> [FestpackScheduleSet] {
        withUnsafeBytes(of: pack.sets) { raw in
            let items = raw.bindMemory(to: fp_set_t.self)
            return (0..<Int(pack.n_sets)).map { i in decodeSet(items[i], id: i, stageIDs: stageIDs) }
        }
    }

    /// Shared with `FestpackSchedule.swift`: decodes one `fp_set_t` the
    /// same way regardless of whether it came straight off a freshly
    /// parsed `fp_pack_t` (here) or out of an `ff_sched_*` result row
    /// (a transient, re-encoded `fp_pack_t` — see that file's own doc
    /// comment on why re-encoding, not a stored pointer, is how this
    /// bridge calls into `ff_sched`). Not `private` for that reason.
    static func decodeSet(_ s: fp_set_t, id: Int, stageIDs: [String]) -> FestpackScheduleSet {
        let stageID = (s.stage_idx >= 0 && Int(s.stage_idx) < stageIDs.count) ? stageIDs[Int(s.stage_idx)] : nil
        return FestpackScheduleSet(
            id: id,
            artist: FixedCString.decode(s.artist),
            stageID: stageID,
            nightDayOfYear: Int(s.day_doy),
            startMinute: s.start_min < 0 ? nil : Int(s.start_min),
            endMinute: s.end_min < 0 ? nil : Int(s.end_min),
            note: FixedCString.decode(s.note))
    }

    private static func decodeFeatures(_ pack: fp_pack_t, stageIDs: [String]) -> [FestpackFeature] {
        withUnsafeBytes(of: pack.features) { raw in
            let items = raw.bindMemory(to: fp_feature_t.self)
            return (0..<Int(pack.n_features)).map { i in
                let f = items[i]
                let stageID = (f.stage_idx >= 0 && Int(f.stage_idx) < stageIDs.count) ? stageIDs[Int(f.stage_idx)] : nil
                let points: [FestpackPoint] = withUnsafeBytes(of: f.pts_en) { ptsRaw in
                    let floats = ptsRaw.bindMemory(to: Float.self)
                    return (0..<Int(f.n_pts)).map { p in
                        FestpackPoint(eastMeters: Double(floats[p * 2]), northMeters: Double(floats[p * 2 + 1]))
                    }
                }
                return FestpackFeature(id: i, kind: FestpackFeatureKind(raw: f.kind), stageID: stageID,
                                        label: FixedCString.decode(f.label), points: points)
            }
        }
    }

    private static func decodeLandmarks(_ pack: fp_pack_t) -> [FestpackLandmark] {
        withUnsafeBytes(of: pack.landmarks) { raw in
            let items = raw.bindMemory(to: fp_landmark_t.self)
            return (0..<Int(pack.n_landmarks)).map { i in
                let lm = items[i]
                let position = lm.has_pos ? FestpackPoint(eastMeters: Double(lm.east_m), northMeters: Double(lm.north_m)) : nil
                return FestpackLandmark(id: FixedCString.decode(lm.id), name: FixedCString.decode(lm.name), position: position)
            }
        }
    }

    /// `fp_meta_t` (2026-09-11 S05 amendment). `sources` is a fixed
    /// `char[FP_MAX_META_SOURCES][FP_META_SOURCE_LEN]` array — imported
    /// as a nested tuple, decoded here the same fixed-byte-slab way
    /// `decodeStages`/etc. decode their own C arrays, one
    /// `FP_META_SOURCE_LEN`-byte slice per source.
    private static func decodeMeta(_ pack: fp_pack_t) -> FestpackMeta {
        guard pack.meta.present else { return .empty }
        let sources: [String] = withUnsafeBytes(of: pack.meta.sources) { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            let stride = Int(FP_META_SOURCE_LEN)
            return (0..<Int(pack.meta.n_sources)).map { i in
                let slice = bytes[(i * stride)..<((i + 1) * stride)]
                let len = slice.firstIndex(of: 0).map { $0 - slice.startIndex } ?? slice.count
                return String(decoding: slice.prefix(len), as: UTF8.self)
            }
        }
        func nonEmpty(_ s: String) -> String? { s.isEmpty ? nil : s }
        return FestpackMeta(
            present: true,
            updated: nonEmpty(FixedCString.decode(pack.meta.updated)),
            sources: sources,
            completeLineup: nonEmpty(FixedCString.decode(pack.meta.complete_lineup)),
            completeSetTimes: nonEmpty(FixedCString.decode(pack.meta.complete_set_times)),
            completeMap: nonEmpty(FixedCString.decode(pack.meta.complete_map)))
    }
}

extension FestpackFeatureKind {
    init(raw: UInt8) {
        switch fp_feature_kind_t(rawValue: UInt32(raw)) {
        case FP_KIND_STAGE: self = .stage
        case FP_KIND_CAMPING: self = .camping
        case FP_KIND_WATER: self = .water
        case FP_KIND_PATH: self = .path
        case FP_KIND_ENTRANCE: self = .entrance
        case FP_KIND_VENDOR: self = .vendor
        case FP_KIND_MEDICAL: self = .medical
        case FP_KIND_POI: self = .poi
        default: self = .unknown
        }
    }
}
