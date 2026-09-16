//
//  CrewPresenceTelemetryTransition.swift — A04: the pure "seen or lost?"
//  decision behind `AppGraph.checkCrewPresenceTransitions(freshRssi:from:)`,
//  split out the same way `TelemetryBatchPolicy` splits Firestore's
//  flush decision from the actual upload — so the TRANSITION TABLE
//  itself is unit-testable with no radio, no real elapsed time, and no
//  `@MainActor` graph at all.
//
//  `HeardPresence` (`Bridge/CrewStore.swift`) is the same HEARD axis
//  `PresenceTag`/`InboxViewModel` already render for a crew member row
//  — `.heard`/`.stale`/`.lost`/`.never` — the S24 "No signal" vocabulary
//  (`PresenceTag.heardLostMS`). This type does not read a clock or a
//  threshold itself; it only compares two ALREADY-CLASSIFIED readings.
//
public enum CrewPresenceTelemetryTransition: Equatable {
    /// A member's presence just read `.heard` for the first time this
    /// graph has watched them (`previous == nil`) or for the first time
    /// since it last read `.lost` (a genuine re-appearance) — never for
    /// a `.heard` -> `.heard` re-read (a node-info replay, another
    /// packet while already `.heard`), which is not a transition at
    /// all.
    case seen
    /// A member's presence just crossed INTO `.lost` ("No signal") from
    /// anything else — never re-fired while it stays `.lost`.
    case lost

    public static func decide(previous: HeardPresence?, current: HeardPresence) -> CrewPresenceTelemetryTransition? {
        guard previous != current else { return nil }
        if current == .heard, previous == nil || previous == .lost { return .seen }
        // `previous == nil` deliberately does NOT fire `.lost`: this
        // graph never actually observed this member CROSS the
        // threshold — it may have been `.lost` for hours before this
        // process started watching — so reporting one now would claim a
        // "just went quiet" moment that never happened. Only a
        // known PRIOR non-lost reading (`.heard`/`.stale`) makes the
        // crossing itself something this graph actually witnessed.
        if current == .lost, let previous, previous != .lost { return .lost }
        return nil
    }
}
