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
            if message.kind == .flare {
                HStack(spacing: 6) {
                    Image(systemName: "flame.fill")
                    Text(flareLabel)
                }
                .font(.system(.callout, design: .rounded).weight(.bold))
                .foregroundStyle(Color.ffBackground)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(Color.ffAmber, in: RoundedRectangle(cornerRadius: 14))
            } else {
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
                            .background(Color.ffSurface, in: Capsule())
                    }
                    .buttonStyle(.plain)
                }
                Button {
                    Task { await model.sendFlare() }
                } label: {
                    Label("FLARE", systemImage: "flame.fill")
                        .font(.system(.caption, design: .rounded).weight(.bold))
                        .foregroundStyle(Color.ffBackground)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .background(Color.ffAmber, in: Capsule())
                }
                .buttonStyle(.plain)
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
            }
            .buttonStyle(.plain)
            .disabled(model.composeText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Color.ffBackground)
    }
}
