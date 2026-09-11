//
//  CoreClock.swift — the millisecond convention every Bridge/* type
//  uses for "now", and the `ff_clock_t` lifetime handling
//  docs/specs/A01-companion-app.md calls out as the sharp edge:
//
//    "`ff_clock_t` is the sharp edge. `ff_crew_init(c, clock)` keeps
//    `ff_clock_t const *clock` — a borrowed pointer with a lifetime
//    requirement the C header states and the compiler cannot enforce.
//    So the clock struct is heap-allocated alongside the context, by
//    the same owner, and freed after it. Its `now_ms` is a
//    `@convention(c)` function (no captures possible) and its `user`
//    is `Unmanaged.passUnretained(owner).toOpaque()` — unretained
//    deliberately, because the owner outlives the clock by
//    construction and a retain here would be a cycle."
//
//  `ff_crew_t` is the ONLY context in `firmware/core` that stores a
//  clock pointer beyond the one call it was handed on — see ff_crew.h's
//  own top comment ("every 'now' the module needs is either passed
//  explicitly by the caller or read once via the injected `ff_clock_t`
//  ... never cached beyond that call" — except `ff_crew_on_rssi`, which
//  is exactly why `ff_crew_t` keeps the pointer at all). `ff_radar_compute`,
//  `ff_feed_*` and `ff_find_*` all take `now_ms` explicitly on every
//  call and need no clock of their own.
//
import FireflyCore
import Foundation

/// The millisecond convention every Bridge/* type and its callers use
/// for "now": epoch milliseconds, truncated to 32 bits. `ff_clock_t`'s
/// own documented convention (ff_clock.h's `ff_time_reached`) is
/// wraparound-safe unsigned-subtraction comparison — correct as long as
/// the true gap between two readings stays under ~24.8 days, which
/// covers every session this app has (a live FIND session tops out at
/// 5 minutes; a crew member ages into LOST after 20 minutes; nothing
/// here runs for weeks without a relaunch).
public enum FireflyClock {
    public static func nowMillis() -> UInt32 { millis(since: Date()) }

    public static func millis(since date: Date) -> UInt32 {
        UInt32(truncatingIfNeeded: Int64((date.timeIntervalSince1970 * 1000).rounded()))
    }
}

/// Owns exactly one heap-allocated `ff_clock_t`, wired to a Swift
/// closure via an unretained `Unmanaged` back-reference to itself (the
/// `@convention(c)` function pointer C requires cannot capture
/// anything, so the closure reads "now" by dereferencing `user`
/// instead). `CrewStore` heap-allocates one of these alongside its own
/// `ff_crew_t` and keeps it alive for exactly as long as that context
/// does — the same owner, freed after the context that borrows it, per
/// the deviation note above.
final class CoreClock {
    private let storage: UnsafeMutablePointer<ff_clock_t>
    private let now: () -> UInt32

    init(now: @escaping () -> UInt32 = FireflyClock.nowMillis) {
        self.now = now
        storage = UnsafeMutablePointer<ff_clock_t>.allocate(capacity: 1)
        let cNowMs: @convention(c) (UnsafeMutableRawPointer?) -> UInt32 = { user in
            guard let user else { return 0 }
            return Unmanaged<CoreClock>.fromOpaque(user).takeUnretainedValue().now()
        }
        storage.initialize(to: ff_clock_t(now_ms: cNowMs, user: nil))
        // Deliberately unretained (see this file's top comment): this
        // object cannot legitimately outlive its own `storage`, so a
        // retain here would only create a cycle, never prevent a
        // use-after-free.
        storage.pointee.user = Unmanaged.passUnretained(self).toOpaque()
    }

    /// Handed to `ff_crew_init`. Valid exactly as long as this
    /// `CoreClock` is alive — never escapes past `CrewStore`, its sole
    /// owner.
    var raw: UnsafePointer<ff_clock_t> { UnsafePointer(storage) }

    deinit {
        storage.deinitialize(count: 1)
        storage.deallocate()
    }
}
