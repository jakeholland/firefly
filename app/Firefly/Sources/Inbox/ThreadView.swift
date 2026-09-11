//
//  ThreadView.swift — one conversation's thread: bubbles, delivery
//  state, quick-reply chips and a real-keyboard compose bar
//  (docs/specs/A01-companion-app.md, slice E; docs/specs/
//  S24-signals-inbox.md).
//
import FireflyMesh
import FireflyModel
import SwiftUI

struct ThreadContainerView: View {
    let model: ThreadViewModel

    var body: some View {
        ThreadView(model: model)
            .onAppear { model.observe() }
            .onDisappear { model.stopObserving() }
    }
}

struct ThreadView: View {
    @Bindable var model: ThreadViewModel

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 10) {
                        ForEach(model.messages) { message in
                            MessageBubble(message: message,
                                          renderedState: model.renderedDeliveryState(for: message),
                                          onResend: { Task { await model.resend(message) } })
                                .id(message.id)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.top, 12)
                }
                .onChange(of: model.messages.count) {
                    if let last = model.messages.last { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }

            if !model.isLinkReady {
                LinkDownBanner(queuedCount: model.queuedCount)
            }
            if let failure = model.immediateSendFailure {
                ImmediateSendFailureBanner(failure: failure)
            }

            QuickReplyRow(model: model)
            ComposeBar(model: model)
        }
        .background(Color.ffBackground)
        .navigationTitle(title)
        #if os(iOS)
        .navigationBarTitleDisplayMode(.inline)
        #endif
    }

    private var title: String {
        switch model.conversation {
        case .crew: return "CREW"
        case .member: return "THREAD"
        }
    }
}

private struct LinkDownBanner: View {
    let queuedCount: Int
    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "antenna.radiowaves.left.and.right.slash")
            Text(queuedCount > 0 ? "NODE NOT CONNECTED · \(queuedCount) QUEUED" : "NODE NOT CONNECTED")
        }
        .font(.system(.caption, design: .monospaced).weight(.semibold))
        .foregroundStyle(Color(fireflyHex: FireflyTheme.staleAmber))
        .frame(maxWidth: .infinity)
        .padding(.vertical, 6)
        .background(Color.ffSurface)
    }
}

/// The transient, non-queued failure banner for a quick-reply or FLARE
/// tap (BLOCKING review item 3: neither is ever queued, so a link-down
/// or transport-error tap needs to fail VISIBLY here instead).
private struct ImmediateSendFailureBanner: View {
    let failure: ImmediateSendFailure

    private var label: String {
        switch failure {
        case .linkDown: return "NOT SENT · NODE NOT CONNECTED"
        case .transportError: return "NOT SENT · TRY AGAIN"
        case .flareUnavailable: return ThreadViewModel.flareUnavailableLabel.uppercased()
        case .rallyNoFix: return "NOT SENT · NO GPS FIX OF YOUR OWN"
        }
    }

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "exclamationmark.triangle.fill")
            Text(label)
        }
        .font(.system(.caption, design: .monospaced).weight(.semibold))
        .foregroundStyle(Color(fireflyHex: FireflyTheme.staleAmber))
        .frame(maxWidth: .infinity)
        .padding(.vertical, 6)
        .background(Color.ffSurface)
    }
}

private struct MessageBubble: View {
    let message: FeedMessage
    let renderedState: DeliveryState?
    let onResend: () -> Void

    private var isMine: Bool { message.direction == .out }

    var body: some View {
        HStack {
            if isMine { Spacer(minLength: 40) }
            VStack(alignment: isMine ? .trailing : .leading, spacing: 3) {
                if !isMine, let name = message.senderName {
                    Text(name.uppercased())
                        .font(.system(.caption2, design: .rounded).weight(.bold))
                        .foregroundStyle(Color.ffMuted)
                }
                bubbleBody
                HStack(spacing: 6) {
                    Text(InboxAge.short(Date().timeIntervalSince(message.timestamp)))
                        .font(.system(.caption2, design: .monospaced))
                        .foregroundStyle(Color.ffMuted)
                    if isMine, let state = renderedState {
                        DeliveryStatusTag(state: state)
                        if state == .noAck || state == .dropped {
                            Button("RESEND", action: onResend)
                                .font(.system(.caption2, design: .monospaced).weight(.bold))
                                .foregroundStyle(Color.ffAmber)
                                .buttonStyle(.plain)
                                // 44pt minimum tap target (SHOULD-FIX 7),
                                // without inflating the caption text's own
                                // visual size.
                                .frame(minWidth: 44, minHeight: 44)
                                .contentShape(Rectangle())
                        }
                    }
                }
            }
            if !isMine { Spacer(minLength: 40) }
        }
    }

    @ViewBuilder
    private var bubbleBody: some View {
        Group {
            switch message.kind {
            case .flare:
                HStack(spacing: 6) {
                    Image(systemName: "flame.fill")
                    Text(flareLabel)
                }
                .font(.system(.callout, design: .rounded).weight(.bold))
                .foregroundStyle(Color.ffBackground)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(Color.ffAmber, in: RoundedRectangle(cornerRadius: 14))
            case .rally:
                // M2: `message.text` already carries the honest
                // distance/bearing suffix, baked in at receive time by
                // `AppGraph.formatRallyText(...)` — never a live-updating
                // number (see that function's own doc comment on why
                // `ff_feed_item_t` has nowhere else to keep it).
                HStack(spacing: 6) {
                    Image(systemName: "mappin.and.ellipse")
                    Text(message.text.isEmpty ? "RALLY" : message.text.uppercased())
                }
                .font(.system(.callout, design: .rounded).weight(.bold))
                .foregroundStyle(isMine ? Color.ffBackground : Color.ffInk)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(isMine ? Color.ffAmber : Color.ffSurface, in: RoundedRectangle(cornerRadius: 14))
            case .status:
                HStack(spacing: 6) {
                    Image(systemName: "text.bubble")
                    Text(message.text)
                }
                .font(.system(.callout, design: .rounded).weight(.semibold))
                .foregroundStyle(isMine ? Color.ffBackground : Color.ffInk)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(isMine ? Color.ffAmber : Color.ffSurface, in: RoundedRectangle(cornerRadius: 14))
            case .text:
                Text(message.text)
                    .font(.body)
                    .foregroundStyle(isMine ? Color.ffBackground : Color.ffInk)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(isMine ? Color.ffAmber : Color.ffSurface,
                                in: RoundedRectangle(cornerRadius: 14))
            }
        }
    }

    private var flareLabel: String {
        if let duration = message.flareDurationSeconds {
            return "FLARE · \(duration)s"
        }
        return "FLARE"
    }
}

private struct QuickReplyRow: View {
    let model: ThreadViewModel

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(ThreadViewModel.quickReplies) { reply in
                    Button {
                        Task { await model.tap(reply) }
                    } label: {
                        Text(reply.label.uppercased())
                            .font(.system(.caption, design: .rounded).weight(.bold))
                            .foregroundStyle(Color.ffInk)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 8)
                            .frame(minWidth: 44, minHeight: 44) // SHOULD-FIX 7
                            .background(Color.ffSurface, in: Capsule())
                    }
                    .buttonStyle(.plain)
                    .contentShape(Capsule())
                }
                // Disabled with an honest label, never a placeholder
                // transmission, whenever no FLARE seam was injected
                // (BLOCKING review item 2).
                Button {
                    Task { await model.sendFlare() }
                } label: {
                    Label("FLARE", systemImage: "flame.fill")
                        .font(.system(.caption, design: .rounded).weight(.bold))
                        .foregroundStyle(model.flareAvailable ? Color.ffBackground : Color.ffMuted)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .frame(minWidth: 44, minHeight: 44) // SHOULD-FIX 7
                        .background(model.flareAvailable ? Color.ffAmber : Color.ffSurface, in: Capsule())
                }
                .buttonStyle(.plain)
                .contentShape(Capsule())
                .disabled(!model.flareAvailable)
                .accessibilityLabel(model.flareAvailable ? "Flare" : ThreadViewModel.flareUnavailableLabel)
                .help(model.flareAvailable ? "" : ThreadViewModel.flareUnavailableLabel)

                // M2: RALLY — "meet at" with our current position
                // (`ThreadViewModel.sendRally(name:)`'s own honesty
                // guard: no fix, no send, never a fabricated lat/lon).
                // Uses whatever is typed in the compose bar as the place
                // label, "MY SPOT" if it's empty.
                Button {
                    Task { await model.sendRally() }
                } label: {
                    Label("RALLY", systemImage: "mappin.and.ellipse")
                        .font(.system(.caption, design: .rounded).weight(.bold))
                        .foregroundStyle(model.rallyAvailable ? Color.ffBackground : Color.ffMuted)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .frame(minWidth: 44, minHeight: 44) // SHOULD-FIX 7
                        .background(model.rallyAvailable ? Color.ffAmber : Color.ffSurface, in: Capsule())
                }
                .buttonStyle(.plain)
                .contentShape(Capsule())
                .disabled(!model.rallyAvailable)
                .accessibilityLabel(model.rallyAvailable ? "Rally" : ThreadViewModel.rallyUnavailableLabel)
                .help(model.rallyAvailable ? "" : ThreadViewModel.rallyUnavailableLabel)
            }
            .padding(.horizontal, 12)
        }
        .padding(.vertical, 6)
    }
}

/// A real keyboard, not T9 (A01: "T9... Out, permanently. A phone has a
/// keyboard").
private struct ComposeBar: View {
    @Bindable var model: ThreadViewModel

    var body: some View {
        HStack(spacing: 8) {
            TextField("Message", text: $model.composeText, axis: .vertical)
                .textFieldStyle(.plain)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(Color.ffSurface, in: RoundedRectangle(cornerRadius: 16))
                .foregroundStyle(Color.ffInk)
                .lineLimit(1...4)
                .onSubmit { Task { await model.sendCompose() } }

            Button {
                Task { await model.sendCompose() }
            } label: {
                Image(systemName: "arrow.up.circle.fill")
                    .font(.system(size: 30))
                    .foregroundStyle(model.composeText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                                      ? Color.ffMuted : Color.ffAmber)
                    .frame(minWidth: 44, minHeight: 44) // SHOULD-FIX 7 — the glyph stays 30pt, the tap target doesn't
            }
            .buttonStyle(.plain)
            .contentShape(Rectangle())
            .disabled(model.composeText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Color.ffBackground)
    }
}
