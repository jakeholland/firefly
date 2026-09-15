//
//  FireflyDebugCrewStateLaunchTests.swift — `-FireflyDebugForgetCrewMembers`
//  / `-FireflyDebugSetCrewProfile <code>`, the bench seam that puts this
//  machine into "on a crew, roster empty" so the next real packet is a
//  first-ever packet from an unknown sender (A02 §4.1).
//
//  Same shape and same scope note as `FireflyDebugCrewLaunchTests`: the
//  parsing is pure and injected, `apply` is driven against in-memory
//  stores, and nothing here launches a process or touches a radio.
//  Both functions are `#if DEBUG`-gated and this suite only runs in a
//  DEBUG configuration, so these assertions are about the DEBUG
//  behaviour; the Release behaviour is a compile-time fact.
//
import FireflyModel
import XCTest

final class FireflyDebugCrewStateLaunchTests: XCTestCase {

    func testForgetMembersIsOffUnlessAskedFor() {
        XCTAssertFalse(FireflyDebugCrewStateLaunch.forgetMembersRequested(arguments: ["Firefly"]))
        XCTAssertTrue(FireflyDebugCrewStateLaunch.forgetMembersRequested(
            arguments: ["Firefly", "-FireflyDebugForgetCrewMembers"]))
    }

    func testProfileCodeIsTheArgumentAfterTheFlagAndIsNotNormalisedHere() {
        XCTAssertEqual(
            FireflyDebugCrewStateLaunch.requestedProfileCode(
                arguments: ["Firefly", "-FireflyDebugSetCrewProfile", "fire 8mntt2"]),
            "fire 8mntt2")
        XCTAssertNil(FireflyDebugCrewStateLaunch.requestedProfileCode(arguments: ["Firefly"]))
        XCTAssertNil(FireflyDebugCrewStateLaunch.requestedProfileCode(
            arguments: ["Firefly", "-FireflyDebugSetCrewProfile"]))
        // A following FLAG is not a code.
        XCTAssertNil(FireflyDebugCrewStateLaunch.requestedProfileCode(
            arguments: ["-FireflyDebugSetCrewProfile", "-FireflyAutoConnect"]))
    }

    func testApplyDoesNothingWithoutFlags() {
        let pairing = InMemoryCrewPairingStore()
        pairing.upsert(CrewPairingRecord(nodeID: 2_403_905_316, colorIndex: 0))
        let profiles = InMemoryCrewProfileStore()

        let actions = FireflyDebugCrewStateLaunch.apply(arguments: ["Firefly"], pairing: pairing, profiles: profiles)

        XCTAssertTrue(actions.isEmpty)
        XCTAssertEqual(pairing.records().count, 1, "a launch with no flags changes nothing")
        XCTAssertNil(profiles.load())
    }

    func testForgetMembersClearsEveryPersistedPairedRecord() {
        let pairing = InMemoryCrewPairingStore()
        pairing.upsert(CrewPairingRecord(nodeID: 2_403_905_316, colorIndex: 0))
        pairing.upsert(CrewPairingRecord(nodeID: 48_621_524, colorIndex: 1))

        let actions = FireflyDebugCrewStateLaunch.apply(arguments: ["-FireflyDebugForgetCrewMembers"],
                                                        pairing: pairing, profiles: InMemoryCrewProfileStore())

        XCTAssertTrue(pairing.records().isEmpty)
        XCTAssertEqual(actions, ["[FireflyDebug] forgot 2 persisted crew member(s)"])
    }

    func testSetCrewProfileWritesTheCanonicalCodeOnly() {
        let profiles = InMemoryCrewProfileStore()

        let actions = FireflyDebugCrewStateLaunch.apply(
            arguments: ["-FireflyDebugSetCrewProfile", "fire 8mntt2"],
            pairing: InMemoryCrewPairingStore(), profiles: profiles,
            now: Date(timeIntervalSince1970: 1_789_000_000))

        XCTAssertEqual(profiles.load()?.code, "FIRE-8MNTT2", "§1.2 normalisation, done by CrewCode.parse")
        XCTAssertEqual(profiles.load()?.createdAtMs, 1_789_000_000_000)
        XCTAssertEqual(actions, ["[FireflyDebug] local crew profile set to FIRE-8MNTT2 (no radio write)"])
    }

    /// An unparseable code is REFUSED, not half-applied — the app must
    /// never end up holding a crew profile no `CrewCode` could produce.
    func testAnInvalidCrewCodeIsRefusedRatherThanStored() {
        let profiles = InMemoryCrewProfileStore()

        let actions = FireflyDebugCrewStateLaunch.apply(
            arguments: ["-FireflyDebugSetCrewProfile", "NOPE"],
            pairing: InMemoryCrewPairingStore(), profiles: profiles)

        XCTAssertNil(profiles.load())
        XCTAssertEqual(actions, ["[FireflyDebug] -FireflyDebugSetCrewProfile NOPE: not a valid crew code — ignored"])
    }
}
