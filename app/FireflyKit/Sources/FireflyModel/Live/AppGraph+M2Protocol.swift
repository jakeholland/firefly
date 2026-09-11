//
//  AppGraph+M2Protocol.swift — M2: inbound FLARE/RALLY/STATUS handling
//  and the S29 PING auto-reply, split out of `AppGraph.swift` itself so
//  that shared file's own diff stays small (three other M2 slices touch
//  it in parallel — see the coordinator's own instructions on this
//  worktree).
//
//  Everything here is reached from exactly one place:
//  `AppGraph.handle(private:)`'s switch, in AppGraph.swift.
//
import FireflyCore
import FireflyMesh
import Foundation

extension AppGraph {
    // MARK: - PONG auto-reply (S29 PR 2)

    /// Any puck (or this app) receiving a PING replies once, immediately,
    /// with the RSSI/SNR OUR OWN radio measured on that PING packet —
    /// "they hear us at -xx dBm" (S29: "no pairing filter... a probe is
    /// answered honestly"). Direct-addressed, `want_ack = false`
    /// (`sendPrivate` under the hood, via `packetSender`), never
    /// broadcast.
    ///
    /// Rate-limited to ONE reply per nonce (task's own explicit ask,
    /// stricter than S29's bare "one PONG per PING received" — protects
    /// against a redelivered/duplicated inbound packet triggering a
    /// second reply to the same probe) via `repliedPongNonces`, a
    /// bounded ring exactly like `InMemoryInboxStore.mySentPacketIDs`'s
    /// own shape.
    ///
    /// A PING whose packet carries no RSSI reading (`rssiDbm == nil` —
    /// `mc_rx_meta_t.has_rssi` false, an implausible/malformed radio
    /// reading) gets NO reply at all: `ff_proto.h`'s own doc comment on
    /// `ff_proto_pong_t.rssi_dbm` — "a replier that can't even trust its
    /// own RSSI reading for the packet it is actively replying to has
    /// nothing honest to report."
    func replyToPing(from: UInt32, nonce: UInt32, rssiDbm: Int16?, snrDb: Float?) {
        guard let rssiDbm else { return }
        guard repliedPongNonces.insertIfNew(from: from, nonce: nonce) else { return }
        let hasSNR = snrDb != nil
        Task {
            do {
                try await packetSender.send(.pong(nonce: nonce, rssiDbm: rssiDbm, snrDb: hasSNR ? snrDb : nil),
                                             to: from, wantAck: false)
            } catch {
                let line = "[AppGraph] PONG reply nonce=\(nonce) to=\(from) failed: \(error)\n"
                FileHandle.standardError.write(Data(line.utf8))
            }
        }
    }

    // MARK: - Inbound FLARE (S10)

    /// Foreground: a full-screen takeover (`FlareTakeoverViewModel`).
    /// Backgrounded: "never shown in the background beyond a local
    /// notification" — the takeover view never renders, a notification
    /// fires instead. Either way the feed keeps a record (S10: "DISMISS
    /// -> back, feed item remains" — true here whether or not anyone was
    /// looking when it arrived).
    ///
    /// Gated on `isPairedSender` (PR #271 review, BLOCKING finding 1)
    /// BEFORE any of that — an unpaired or unknown sender gets no
    /// takeover, no haptic, no notification, and no feed item, exactly
    /// the puck's own `wiring_push_if_paired` (`ff_wiring.c`).
    func handleInboundFlare(from: UInt32, to: UInt32, durationS: UInt16) {
        guard isPairedSender(from) else { logDroppedUnpaired("FLARE", from: from); return }
        pushInboundFeedItem(kind: .flare, from: from, to: to, text: "FLARE")
        if isForegrounded {
            flareTakeover.show(senderNodeID: from, durationSeconds: durationS)
        } else {
            let name = core.crew.member(nodeID: from, now: FireflyClock.nowMillis())?.displayName
            let senderName = (name?.isEmpty == false) ? name! : "Someone"
            Task { await notifications.postFlare(senderName: senderName) }
        }
    }

    /// FLARE_END — only ever clears a takeover currently showing FOR
    /// THIS sender (`FlareTakeoverViewModel.end(from:)`'s own guard); no
    /// feed item (mirrors the puck's own FLARE_END, which has never been
    /// a feed-worthy event on either client).
    ///
    /// Gated on `isPairedSender` too, for the same trust-boundary reason
    /// as `handleInboundFlare` — though in practice this is a no-op
    /// either way for an unpaired sender, since a takeover for one could
    /// never have started (`handleInboundFlare` above already dropped
    /// it) for `end(from:)`'s own sender-match guard to have anything to
    /// clear.
    func handleInboundFlareEnd(from: UInt32) {
        guard isPairedSender(from) else { logDroppedUnpaired("FLARE_END", from: from); return }
        flareTakeover.end(from: from)
    }

    // MARK: - Inbound RALLY / RALLY_CLEAR / STATUS (S04)

    /// If we have both our own current fix and the RALLY's own lat/lon,
    /// the feed text carries a real, honestly-computed distance/bearing
    /// ("MY SPOT — 210 m NE of you"); otherwise just the name — never a
    /// fabricated reading. `ff_feed_item_t` has no numeric fields for a
    /// position (S04's RALLY body only ever rides the wire, not the
    /// feed's own storage), so this computed string IS the persisted
    /// record, exactly as the puck's own feed never stores anything but
    /// text either.
    ///
    /// Gated on `isPairedSender` (PR #271 review, BLOCKING finding 1) —
    /// S04's Addressing section: "RALLY/STATUS broadcast likewise" (as
    /// FLARE's own receiver-side crew filtering).
    func handleInboundRally(from: UInt32, to: UInt32, latitude: Double, longitude: Double, name: String) {
        guard isPairedSender(from) else { logDroppedUnpaired("RALLY", from: from); return }
        let text = formatRallyText(name: name, latitude: latitude, longitude: longitude)
        pushInboundFeedItem(kind: .rally, from: from, to: to, text: text)
    }

    /// RALLY_CLEAR carries no place of its own to clear from the feed
    /// (S04's body is empty) — nothing dishonest to render, so nothing
    /// is pushed, mirroring FLARE_END's own "no feed item" treatment.
    /// Already an unconditional no-op, so there is nothing an unpaired
    /// sender could trigger here to gate.
    func handleInboundRallyClear(from: UInt32) {}

    /// Gated on `isPairedSender`, same as `handleInboundRally` — S04's
    /// "RALLY/STATUS broadcast likewise."
    func handleInboundStatus(from: UInt32, to: UInt32, text: String) {
        guard isPairedSender(from) else { logDroppedUnpaired("STATUS", from: from); return }
        pushInboundFeedItem(kind: .status, from: from, to: to, text: text)
    }

    // MARK: - Shared helpers

    /// Read-only crew-pairing check (PR #271 review, BLOCKING finding
    /// 1) — mirrors the puck's own `wiring_push_if_paired`'s lookup
    /// (`ff_wiring.c`): `core.crew.member(nodeID:now:)` calls
    /// `ff_crew_find` under the hood (`CrewStore.member(nodeID:now:)`),
    /// never `ff_crew_upsert`, so merely checking whether a sender is
    /// paired can never itself consume one of the roster's fixed
    /// `FF_CREW_MAX` slots. An unknown sender (`nil`) and a
    /// known-but-unpaired one are both untrusted — S04's Addressing
    /// section: "FLARE to broadcast with crew filtering receiver-side
    /// (only react if sender is paired) ... RALLY/STATUS broadcast
    /// likewise." Must be called BEFORE `pushInboundFeedItem` — that
    /// method's own `inboxProvider.push` upserts the sender into the
    /// roster as a pre-existing side effect, so the pairing check can
    /// never be inferred from having already reached that call.
    func isPairedSender(_ from: UInt32) -> Bool {
        core.crew.member(nodeID: from, now: FireflyClock.nowMillis())?.paired == true
    }

    /// The debug-log-only record of a dropped-for-unpaired-sender M2
    /// event (task's own "dropped silently with a debug log" ask) —
    /// same `FileHandle.standardError.write` convention `replyToPing`
    /// already uses for its own non-fatal error path.
    private func logDroppedUnpaired(_ kind: String, from: UInt32) {
        let line = "[AppGraph] dropping inbound \(kind) from unpaired/unknown sender=\(from)\n"
        FileHandle.standardError.write(Data(line.utf8))
    }

    /// Every inbound M2 feed item goes through here: CREW when the
    /// packet was broadcast, the sender's own 1:1 thread otherwise —
    /// same membership rule `InboxViewModel.ingest(_:)` uses for
    /// ordinary inbound text, via the ONE shared `isBroadcastDestination`
    /// helper both now call (PR #271 review, SHOULD-FIX 2) — no more
    /// divergent `to == 0` special case here that `ingest(_:)` doesn't
    /// also have. `id: 0` is the correct, honest choice for an inbound
    /// item (`OutboxID`'s own "0 = not tracked" sentinel) — unlike an
    /// outbound send, nothing will ever look this item up by outbox id
    /// again; `CoreInboxProvider` derives its real, stable-across-
    /// rebuilds id for an inbound item from the record itself, not from
    /// what gets pushed here.
    func pushInboundFeedItem(kind: MessageKind, from: UInt32, to: UInt32, text: String) {
        let isBroadcast = isBroadcastDestination(to)
        let conversation: ConversationKind = isBroadcast ? .crew : .member(from)
        let message = FeedMessage(id: 0, kind: kind, direction: isBroadcast ? .broadcast : .direct,
                                   senderID: from, text: text, timestamp: Date(), unread: true)
        inboxProvider.push(message, into: conversation)
    }

    /// Refuses a confident-looking bearing off a stale fix of OUR OWN
    /// (PR #271 review, SHOULD-FIX 3) — same `LocationFix.isStale` rule
    /// `FlareTakeoverViewModel.show` now applies, reused rather than a
    /// second staleness number invented here.
    func formatRallyText(name: String, latitude: Double, longitude: Double, now: Date = Date()) -> String {
        guard let fix = myFix else { return name }
        guard !fix.isStale(now: now) else {
            return "\(name) — no bearing (your fix is \(fix.ageMinutesText(now: now)) min old)"
        }
        let from = ff_latlon_t(lat: fix.latitude, lon: fix.longitude)
        let to = ff_latlon_t(lat: latitude, lon: longitude)
        let distance = FlareTakeoverViewModel.formatDistance(Double(ff_geo_distance_m(from, to)))
        let compass = CompassPoint.name(forBearingDegrees: Double(ff_geo_bearing_deg(from, to)))
        return "\(name) — \(distance) \(compass) of you"
    }

    // MARK: - My own current fix (for RALLY distance/bearing, and the
    // FLARE takeover's own bearing) and app-active state

    /// Started from `start()`, cancelled from `stop()` — see
    /// `AppGraph.swift`'s own calls into these two. A single subscription
    /// shared by RALLY rendering and `FlareTakeoverViewModel`, rather
    /// than each keeping an independent one: the phone has exactly one
    /// GPS, and `PhoneGPSUplink` already keeps its own separate
    /// subscription alive for the push-to-node path (EventHub is
    /// multicast — a second, independent subscriber is free, not a
    /// conflict).
    func observeMyLocation() {
        guard locationObservation == nil else { return }
        let fixes = dependencies.location.fixes()
        locationObservation = Task { [weak self] in
            for await fix in fixes {
                guard let self else { return }
                self.myFix = fix
            }
        }
    }

    func stopObservingMyLocation() {
        locationObservation?.cancel()
        locationObservation = nil
    }

    /// A second, independent `incomingTexts()` subscription (EventHub is
    /// multicast, S1) purely to notice a backgrounded arrival and post a
    /// notification — `InboxViewModel` keeps its own, separate
    /// subscription for actually rendering the thread, and the two never
    /// interfere with each other.
    func observeIncomingTextsForNotifications() {
        guard incomingTextNotificationObservation == nil else { return }
        let texts = dependencies.client.incomingTexts()
        incomingTextNotificationObservation = Task { [weak self] in
            for await incoming in texts {
                guard let self, !self.isForegrounded else { continue }
                let name = self.core.crew.member(nodeID: incoming.from, now: FireflyClock.nowMillis())?.displayName
                let senderName = (name?.isEmpty == false) ? name! : "Someone"
                await self.notifications.postMessage(senderName: senderName, preview: incoming.text)
            }
        }
    }

    func stopObservingIncomingTextsForNotifications() {
        incomingTextNotificationObservation?.cancel()
        incomingTextNotificationObservation = nil
    }
}

/// A bounded FIFO of the most recent (from, nonce) pairs already
/// answered — the same shape `InMemoryInboxStore`'s `SentIDRing`/
/// `CoreInboxProvider`'s `PacketIDRing` already use for an identical
/// "remember a bounded number of recent keys" need, keyed on the PAIR
/// here (`from` alone is not unique — the same sender's Nth ping reuses
/// low nonce values across a long session; `nonce` alone is not unique
/// either — two different senders' sessions can coincide).
struct PongReplyDedup {
    static let capacity = 64
    private var order: [UInt64] = []
    private var members: Set<UInt64> = []

    /// Returns true (and remembers the pair) the first time this exact
    /// (from, nonce) pair is seen; false — no state change — on a
    /// repeat, which is the "rate-limit to one reply per nonce" the
    /// caller relies on.
    mutating func insertIfNew(from: UInt32, nonce: UInt32) -> Bool {
        let key = (UInt64(from) << 32) | UInt64(nonce)
        guard !members.contains(key) else { return false }
        order.append(key)
        members.insert(key)
        if order.count > Self.capacity { members.remove(order.removeFirst()) }
        return true
    }
}
