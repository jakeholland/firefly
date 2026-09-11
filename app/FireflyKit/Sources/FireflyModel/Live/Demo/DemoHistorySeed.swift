//
//  DemoHistorySeed.swift — `-FireflyDemoRestored`'s own seed data
//  (docs/specs/A01-companion-app.md, M3).
//
//  Writes a small, plausible "yesterday" history directly into a
//  `HistoryStore` BEFORE `AppGraph.init` ever runs its own restore pass
//  over that same store — so the exact code path a real relaunch takes
//  (`HistoryRestorer.restore`) is what puts these messages on screen,
//  tagged FROM STORAGE with their own honest age, rather than a parallel
//  "looks restored" fake built some other way. `FireflyApp.init` is the
//  one caller: it constructs a fresh `HistoryStore.inMemory()`, calls
//  `seed(into:)`, and hands that store to `AppGraph.init(historyStore:)`
//  — never `.live()`, so this never touches disk (S20's demo-isolation
//  rule, restated for M3: "Demo doesn't persist across launches —
//  in-memory store only").
//
import Foundation

public enum DemoHistorySeed {
    /// Two messages in Taylor's thread, both well before "now": an
    /// inbound text (renders with a plain restored age) and an outbound
    /// one recorded SENT (restores as NO ACK — `HistoryRestorer`'s own
    /// honesty transform — proving that rule is visible in the
    /// screenshot, not just covered by a unit test). Seeded into
    /// Taylor's conversation specifically because `DemoRunner.start()`
    /// pairs Taylor first and opens exactly that thread — the restored
    /// history and the live scripted timeline land in the SAME thread on
    /// purpose, so a screenshot can show both "FROM STORAGE" bubbles and
    /// fresh live ones stacked in one screen.
    @MainActor
    public static func seed(into history: HistoryStore, now: Date = Date()) {
        let yesterdayMorning = now.addingTimeInterval(-18 * 3600)
        let yesterdayAfternoon = now.addingTimeInterval(-16 * 3600 - 20 * 60)

        history.record(
            FeedMessage(id: 0x8000_0000_0000_2001, kind: .text, direction: .direct, senderID: DemoCrew.taylor,
                        senderName: "Taylor", text: "made it to the gate, heading to camp",
                        timestamp: yesterdayMorning, unread: false),
            in: .member(DemoCrew.taylor))

        // Recorded SENT (as it genuinely would have been, mid-session,
        // before the app "closed") — `HistoryRestorer.restore` is what
        // turns this into NO ACK on the way back in; this seed never
        // pre-applies that transform itself, so the demo exercises the
        // real rule rather than assuming it.
        //
        // `id:` is a large, reserved constant — deliberately NOT `1`.
        // `ThreadViewModel`'s own `OutboxIDGenerator.shared` is a
        // process-global singleton that ALSO starts counting outbound
        // ids at 1, and `DemoRunner.sendDemoThreadMessages()` sends two
        // more live compose messages through that exact generator later
        // in this same process — an `id: 1` here would collide with the
        // live send's own outbox id the moment both exist in the same
        // ring, and `markSent`/`setStatus(outboxID:)` would silently
        // update whichever of the two items the C core happens to find
        // first (bench-reproduced while building this: the seeded
        // message's own SENT status flipped to the LIVE send's
        // DELIVERED, not the honest NO ACK M3 promises). Reserved here
        // the same way `InboundFeedIDGenerator` reserves the top bit for
        // every inbound id — out of range of any counter this process's
        // own generators could plausibly reach in one demo run.
        history.record(
            FeedMessage(id: 0x0000_0000_F000_0001, kind: .text, direction: .out,
                        text: "see you at the tower around 6", timestamp: yesterdayAfternoon,
                        destination: DemoCrew.taylor, packetID: 700_501, deliveryState: .sent,
                        statusAt: yesterdayAfternoon),
            in: .member(DemoCrew.taylor))
    }
}
