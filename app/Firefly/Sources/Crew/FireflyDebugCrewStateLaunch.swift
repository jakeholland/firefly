//
//  FireflyDebugCrewStateLaunch.swift — `-FireflyDebugForgetCrewMembers`
//  and `-FireflyDebugSetCrewProfile <code>`: the bench seam for putting
//  this Mac back into the ONE state A02 §4.1's admission rule is
//  actually about — on a crew, with nobody on the roster yet — so that
//  the very next packet from a real puck is a FIRST-EVER packet from an
//  unknown sender.
//
//  Shaped exactly like `FireflyDebugCrewLaunch` next door, and for the
//  same reasons: pure parsing functions over an injectable arguments
//  array, in the APP target, `#if DEBUG`-gated so a Release build
//  compiles them out entirely and always reads `nil`/`false`.
//
//  WHAT THESE DO NOT DO: touch the radio. Neither flag sends an admin
//  message, writes a channel, or reboots anything.
//
//   * `-FireflyDebugForgetCrewMembers` clears the PERSISTED paired list
//     (`CrewPairingStore`) only, and only before `AppGraph.init` can
//     replay it (`CrewPairingRestorer`). It is the local equivalent of
//     never having met anyone — not a "leave", not a hide.
//   * `-FireflyDebugSetCrewProfile <code>` writes the LOCAL crew record
//     A02 §2.1 step 3 describes ("the crew code + human name + creation
//     time a Start/Join writes locally BEFORE any radio write") and
//     stops there. It deliberately does NOT run the radio half of a
//     Join, which is why it is usable against a puck that is already
//     provisioned for that crew and must not be re-written. Nothing
//     about it can make the app claim a crew it is not really on: the
//     channel index is still resolved honestly against the radio's own
//     table (`CrewMembershipEngine.channelStatus`), so a code that does
//     not match anything on the connected node lands on §4.2's
//     "Your puck isn't on this crew's channel", exactly as a mistyped
//     join does.
//
import FireflyModel
import Foundation

enum FireflyDebugCrewStateLaunch {
    /// `-FireflyDebugForgetCrewMembers` — drop every persisted paired
    /// crew member, so the next packet from each of them is a
    /// first-ever packet from an unknown sender. `false` on every
    /// ordinary launch, and unconditionally `false` outside `DEBUG`.
    static func forgetMembersRequested(arguments: [String] = CommandLine.arguments) -> Bool {
        #if DEBUG
        arguments.contains("-FireflyDebugForgetCrewMembers")
        #else
        false
        #endif
    }

    /// `-FireflyDebugSetCrewProfile <code>` — the crew code to record
    /// locally. Returns the raw argument; PARSING is the caller's
    /// business (same division `FireflyDebugCrewLaunch
    /// .requestedJoinCode()` documents), so an unparseable code is
    /// rejected by `CrewCode.parse` rather than half-applied here.
    static func requestedProfileCode(arguments: [String] = CommandLine.arguments) -> String? {
        #if DEBUG
        guard let index = arguments.firstIndex(of: "-FireflyDebugSetCrewProfile"),
              index + 1 < arguments.count,
              !arguments[index + 1].hasPrefix("-") else { return nil }
        return arguments[index + 1]
        #else
        nil
        #endif
    }

    /// Applies whichever of the two was asked for, to the REAL persisted
    /// stores, and returns a line per action taken (logged by the
    /// caller, so a bench run's stderr says exactly what was changed and
    /// a run with no flags says nothing at all).
    ///
    /// Must be called BEFORE `AppGraph.init`: that is where
    /// `CrewPairingRestorer.restore` replays the paired list onto
    /// `ff_crew` and where `syncCrewMembershipWithProfile()` reads the
    /// profile. Applying after would leave the live roster and the
    /// persisted one disagreeing.
    @discardableResult
    static func apply(arguments: [String] = CommandLine.arguments,
                      pairing: any CrewPairingStoring = CrewPairingStore(),
                      profiles: any CrewProfileStoring = CrewProfileStore(),
                      now: Date = Date()) -> [String] {
        var actions: [String] = []
        #if DEBUG
        if forgetMembersRequested(arguments: arguments) {
            let existing = pairing.records()
            for record in existing { pairing.remove(nodeID: record.nodeID) }
            actions.append("[FireflyDebug] forgot \(existing.count) persisted crew member(s)")
        }
        if let raw = requestedProfileCode(arguments: arguments) {
            if let code = try? CrewCode.parse(raw).canonical {
                profiles.save(CrewProfile(code: code, humanName: "Bench crew",
                                           createdAtMs: UInt64((now.timeIntervalSince1970 * 1000).rounded())))
                actions.append("[FireflyDebug] local crew profile set to \(code) (no radio write)")
            } else {
                actions.append("[FireflyDebug] -FireflyDebugSetCrewProfile \(raw): not a valid crew code — ignored")
            }
        }
        #endif
        return actions
    }
}
