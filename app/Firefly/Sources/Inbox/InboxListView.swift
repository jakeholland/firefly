//
//  InboxListView.swift — the Inbox screen: CREW + one row per paired
//  crew member (docs/specs/A01-companion-app.md, slice E;
//  docs/specs/S24-signals-inbox.md).
//
import FireflyMesh
import FireflyModel
import SwiftUI

/// The Inbox destination's whole navigation: the conversation list,
/// pushing to a thread on tap. Owns its own `NavigationStack` so it
/// drops into either platform's `RootView` shape (a `NavigationSplitView`
/// detail pane on macOS, a `TabView` tab on iOS) without either one
/// needing to know it pushes.
struct InboxContainerView: View {
    let model: InboxViewModel
    /// Demo-only (`-FireflyDemoScreen thread`, `RootView`'s own
    /// mapping): pushes straight to this conversation's thread on
    /// appear, through the SAME `navigationDestination(item:)` a real
    /// tap uses — never a second, parallel presentation path — so the
    /// "Thread with the delivery states" screenshot is the real Thread
    /// screen, not a stand-in. `nil` in every non-demo build.
    var demoInitialThread: ConversationKind?
    /// `SettingsViewModel.colorblindPalette` (M2) — threaded down to
    /// every avatar/swatch this screen and its Thread destination
    /// render, the SAME flag Radar's ring reads, so a member's colour
    /// never disagrees between the two faces.
    var colorblind: Bool = false
    @State private var activeThread: ThreadViewModel?

    var body: some View {
        NavigationStack {
            InboxListView(model: model, colorblind: colorblind) { kind in
                activeThread = model.openThread(kind)
            }
            .navigationTitle("INBOX")
            .background(Color.ffBackground)
            .navigationDestination(item: $activeThread) { thread in
                ThreadContainerView(model: thread, colorblind: colorblind)
            }
        }
        // `model.observe()`/`model.stopObserving()` are deliberately NOT
        // called here any more — `AppGraph.makeInboxViewModel()` starts
        // `observe()` once, for the life of the graph. Same fix,
        // identical NavigationSplitView detail-column remount hazard, as
        // `ConnectScreen.swift`'s own `.onAppear` comment documents (this
        // screen is one of `RootView`'s own `detail(for:)` destinations,
        // same as Connect). `ThreadViewModel`, opened fresh per
        // `model.openThread(_:)` call below and pushed through this
        // view's own nested `NavigationStack`, is unaffected — that is
        // genuinely per-navigation state, not a `detail(for:)`
        // destination — and keeps its own `.onAppear`/`.onDisappear` in
        // `ThreadView.swift`.
        .task {
            guard let demoInitialThread, activeThread == nil else { return }
            activeThread = model.openThread(demoInitialThread)
        }
    }
}

struct InboxListView: View {
    let model: InboxViewModel
    var colorblind: Bool = false
    let onSelect: (ConversationKind) -> Void

    /// S24's "no crew paired" edge state: only the CREW row exists.
    private var noCrewPaired: Bool {
        model.conversations.allSatisfy { $0.kind == .crew }
    }

    var body: some View {
        List {
            ForEach(model.conversations) { conversation in
                Button { onSelect(conversation.kind) } label: {
                    InboxRow(conversation: conversation, colorblind: colorblind)
                }
                .buttonStyle(.plain)
            }
            .listRowBackground(Color.ffBackground)
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .background(Color.ffBackground)
        .overlay {
            if noCrewPaired {
                VStack(spacing: 8) {
                    Text("NO CREW LINKED YET")
                        .font(.system(.headline, design: .rounded).weight(.bold))
                        .foregroundStyle(Color.ffInk)
                    Text("Pair crew from the Crew screen to start a conversation.")
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)
                }
                .allowsHitTesting(false)
            }
        }
    }
}

private struct InboxRow: View {
    let conversation: InboxConversationRow
    let colorblind: Bool

    var body: some View {
        HStack(spacing: 12) {
            avatar
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(conversation.displayName)
                        .font(.system(.body, design: .rounded).weight(.semibold))
                        .foregroundStyle(Color.ffInk)
                    if let presence = conversation.presence {
                        PresencePill(presence: presence, age: conversation.presenceAge)
                    }
                    Spacer(minLength: 0)
                    if let age = conversation.previewAge {
                        Text(InboxAge.short(age))
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(Color.ffMuted)
                    }
                }
                HStack(spacing: 6) {
                    Text(previewLine)
                        .font(.subheadline)
                        .foregroundStyle(conversation.hasPreview ? Color.ffMuted : Color.ffMuted.opacity(0.6))
                        .lineLimit(1)
                    Spacer(minLength: 0)
                    if let state = conversation.previewDeliveryState {
                        DeliveryStatusTag(state: state)
                    }
                }
            }
            if conversation.unreadCount > 0 {
                UnreadBadge(count: conversation.unreadCount)
            }
        }
        .padding(.vertical, 4)
        .contentShape(Rectangle())
    }

    private var previewLine: String {
        guard conversation.hasPreview else {
            return conversation.kind == .crew ? "no signals yet" : "no messages yet"
        }
        let text = InboxText.preview(conversation.previewText)
        if conversation.previewDirection == .out { return "You: \(text)" }
        if let from = conversation.previewFromName, conversation.kind == .crew { return "\(from): \(text)" }
        return text
    }

    @ViewBuilder
    private var avatar: some View {
        // `RadarCrewPalette.hex(index:colorblind:)`, not
        // `FireflyTheme.crewColor(index:)` directly (M2) — the SAME
        // palette-selector Radar's ring uses, so a member's avatar here
        // never disagrees with their dot there.
        let color = conversation.colorIndex
            .map { Color(fireflyHex: RadarCrewPalette.hex(index: $0, colorblind: colorblind)) } ?? Color.ffAmber
        ZStack {
            Circle().fill(color.opacity(0.24))
            if conversation.kind == .crew {
                Image(systemName: "person.3.fill")
                    .foregroundStyle(color)
            } else {
                // The "no `Character("")` trap" fix — see
                // `InboxAvatar.avatarGlyph(for:)`'s own doc comment
                // (FireflyModel) for why an empty `displayName` is a
                // real, reachable state and why a blank glyph, not a
                // crash or a "?" placeholder, is correct here. Pulled
                // into that pure helper so the fix is unit-testable
                // (`InboxAvatarTests`, FireflyAppTests) rather than only
                // eyeballed in this view.
                Text(InboxAvatar.avatarGlyph(for: conversation))
                    .font(.system(.callout, design: .rounded).weight(.bold))
                    .foregroundStyle(color)
            }
        }
        .frame(width: 40, height: 40)
    }
}

private struct PresencePill: View {
    let presence: PresenceTag
    let age: TimeInterval?

    private var color: Color {
        switch presence {
        case .heard: return .ffLiveGreen
        case .stale: return Color(fireflyHex: FireflyTheme.staleAmber)
        case .lost: return .ffMuted
        case .linked: return .ffMuted
        }
    }

    private var label: String {
        switch presence {
        case .heard, .stale:
            guard let age else { return presence.rawValue }
            return "\(presence.rawValue) \(InboxAge.short(age))"
        case .lost, .linked:
            return presence.rawValue
        }
    }

    var body: some View {
        Text(label)
            .font(.system(.caption2, design: .monospaced).weight(.semibold))
            .foregroundStyle(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(color.opacity(0.14), in: Capsule())
    }
}

struct DeliveryStatusTag: View {
    let state: DeliveryState

    private var color: Color {
        switch state {
        case .delivered: return .ffLiveGreen
        case .noAck, .dropped: return Color(fireflyHex: FireflyTheme.staleAmber)
        case .waiting, .sent: return .ffMuted
        }
    }

    private var label: String {
        switch state {
        case .waiting: return "WAITING"
        case .sent: return "SENT"
        case .delivered: return "DELIVERED"
        case .noAck: return "NO ACK"
        case .dropped: return "DROPPED"
        }
    }

    var body: some View {
        Text(label)
            .font(.system(.caption2, design: .monospaced).weight(.semibold))
            .foregroundStyle(color)
    }
}

private struct UnreadBadge: View {
    let count: Int
    var body: some View {
        Text("\(count)")
            .font(.system(.caption2, design: .rounded).weight(.bold))
            .foregroundStyle(Color.ffBackground)
            .padding(.horizontal, 6)
            .padding(.vertical, 3)
            .background(Color.ffAmber, in: Capsule())
            .frame(minWidth: 20)
    }
}

/// A short, mono age string for a row — "6M", "2H", "3D". Never a raw
/// second count; the same "words, not measurements you can't back up"
/// spirit `SignalTierPresentation` uses, applied to time instead of
/// signal strength.
enum InboxAge {
    static func short(_ interval: TimeInterval) -> String {
        let seconds = max(0, Int(interval))
        if seconds < 60 { return "NOW" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes)M" }
        let hours = minutes / 60
        if hours < 24 { return "\(hours)H" }
        return "\(hours / 24)D"
    }
}
