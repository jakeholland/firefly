//
//  TelemetryRecorder.swift — A04 (docs/specs/A04-telemetry.md): the
//  durable, offline-first heart of field-test telemetry. "Make sure we
//  don't miss any data" (the owner's own ask) is answered HERE, not by
//  any sink: every event is appended to a local JSON-lines file before
//  anything is fanned out, so a disconnected puck, a dead phone
//  network, or Firebase never being configured at all still leaves a
//  complete local record behind.
//
//  File shape: `<directory>/telemetry.jsonl` is the file currently
//  being appended to; `telemetry.1.jsonl` is the most recently rotated
//  one, up to `telemetry.<maxFiles - 1>.jsonl` — oldest kept, one line
//  per event, newest line last. Rotation happens the moment appending a
//  line would push the CURRENT file over `maxFileBytes` (~5 MB by
//  default); the oldest rotated file beyond `maxFiles` total is deleted.
//
//  Durability across a relaunch: this actor never truncates an existing
//  current file on `init` — the first `record(_:)` call after a fresh
//  launch reads the existing file's size (if any) and keeps appending
//  to it, exactly the file a PREVIOUS process instance was writing.
//  `TelemetryRecorderDurabilityTests` constructs two separate instances
//  over the same directory to pin this.
//
import Foundation

public actor TelemetryRecorder: TelemetryRecording, TelemetryExporting, TelemetrySinkAttaching {
    private let directory: URL
    private let baseFilename: String
    private let maxFileBytes: Int
    /// Total files kept, current included — `1` would mean "current
    /// file only, no rotation history at all", which is never useful, so
    /// this is clamped to at least `2` in `init`.
    private let maxFiles: Int
    private let sessionID: String

    private var sinks: [any TelemetrySink]
    private var seq: UInt64 = 0
    /// `nil` until the first `record(_:)` call actually opens (or
    /// creates) the current file — lazily, so constructing this actor
    /// never touches disk on its own (the same "harmless to construct"
    /// convention `BLETransport()`'s own doc comment describes for
    /// `CBCentralManager`).
    private var currentHandle: FileHandle?
    private var currentSize: Int

    /// Raw stderr write, same discipline as `MeshtasticClient.log(_:)`/
    /// `HistoryStore.log(_:)` — never lost to stdout's block buffering.
    private static func log(_ message: String) {
        FileHandle.standardError.write(Data("[TelemetryRecorder] \(message)\n".utf8))
    }

    public init(directory: URL, baseFilename: String = "telemetry", maxFileBytes: Int = 5 * 1024 * 1024,
                maxFiles: Int = 5, sinks: [any TelemetrySink] = [], sessionID: String = UUID().uuidString) {
        self.directory = directory
        self.baseFilename = baseFilename
        self.maxFileBytes = maxFileBytes
        self.maxFiles = max(2, maxFiles)
        self.sinks = sinks
        self.sessionID = sessionID
        self.currentSize = 0
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    // MARK: - TelemetryRecording

    public func record(_ event: TelemetryEvent) async {
        seq += 1
        var stamped = event.stamped(seq: seq, sessionID: sessionID)
        stamped.attributes = TelemetryAttributeAllowlist.strip(stamped.attributes)
        append(stamped)
        for sink in sinks {
            await sink.send(stamped)
        }
    }

    // MARK: - TelemetrySinkAttaching

    public func addSink(_ sink: any TelemetrySink) async {
        sinks.append(sink)
    }

    /// "…and on background" — the composition root's own hook
    /// (`AppGraph.handleScenePhaseChange(.background)`) calls this,
    /// which fans out to every attached sink's own `flushOnBackground()`
    /// (`TelemetrySink`'s own doc comment). The local JSONL needs no
    /// equivalent: `append(_:)` already writes every event durably the
    /// instant `record(_:)` is called, background or not — this exists
    /// purely for a sink (Firestore) that BATCHES before it leaves the
    /// phone.
    public func notifyBackground() async {
        for sink in sinks {
            await sink.flushOnBackground()
        }
    }

    // MARK: - TelemetryExporting

    /// Oldest first, current file last — the order "Export diagnostics"
    /// concatenates them in, so a reader sees the record in the order it
    /// happened even across a rotation boundary.
    public func exportFiles() async -> [URL] {
        var files: [URL] = []
        for index in stride(from: maxFiles - 1, through: 1, by: -1) {
            let url = rotatedURL(index)
            if FileManager.default.fileExists(atPath: url.path) { files.append(url) }
        }
        if FileManager.default.fileExists(atPath: currentURL.path) { files.append(currentURL) }
        return files
    }

    // MARK: - File I/O

    private var currentURL: URL { directory.appending(path: "\(baseFilename).jsonl") }
    private func rotatedURL(_ index: Int) -> URL { directory.appending(path: "\(baseFilename).\(index).jsonl") }

    private func append(_ event: TelemetryEvent) {
        guard let line = encode(event) else {
            Self.log("failed to encode event \(event.name) — dropped, never silently corrupting the file")
            return
        }
        if currentHandle == nil {
            openOrCreateCurrentFile()
        }
        let lineBytes = Data((line + "\n").utf8)
        if currentSize > 0, currentSize + lineBytes.count > maxFileBytes {
            rotate()
            openOrCreateCurrentFile()
        }
        guard let handle = currentHandle else { return }
        do {
            try handle.seekToEnd()
            handle.write(lineBytes)
            currentSize += lineBytes.count
        } catch {
            Self.log("write failed: \(error) — event \(event.name) (seq \(event.seq)) was not persisted")
        }
    }

    private func openOrCreateCurrentFile() {
        let url = currentURL
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
            currentSize = 0
        } else if currentSize == 0 {
            // A relaunch: pick up the existing file's real size rather
            // than assuming it is empty, so `append` rotates at the
            // right moment instead of writing well past `maxFileBytes`
            // before its first rotation check of this process.
            let attributes = try? FileManager.default.attributesOfItem(atPath: url.path)
            currentSize = (attributes?[.size] as? Int) ?? 0
        }
        currentHandle = FileHandle(forWritingAtPath: url.path)
    }

    private func rotate() {
        currentHandle?.closeFile()
        currentHandle = nil
        let fm = FileManager.default
        let oldest = rotatedURL(maxFiles - 1)
        try? fm.removeItem(at: oldest)
        for index in stride(from: maxFiles - 2, through: 1, by: -1) {
            let from = rotatedURL(index)
            guard fm.fileExists(atPath: from.path) else { continue }
            try? fm.moveItem(at: from, to: rotatedURL(index + 1))
        }
        if fm.fileExists(atPath: currentURL.path) {
            try? fm.moveItem(at: currentURL, to: rotatedURL(1))
        }
        currentSize = 0
    }

    private func encode(_ event: TelemetryEvent) -> String? {
        guard let data = try? Self.encoder.encode(event) else { return nil }
        return String(data: data, encoding: .utf8)
    }

    private static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        return encoder
    }()
}
