//
//  AdminWriteConfirmationSheet.swift — M3's one confirmation surface,
//  shared by every admin write this app makes: the Connect screen's
//  "Apply to node" (channel/LoRa) and Settings' node-name/region writes
//  (docs/specs/A01-companion-app.md, M3: "Channel write-back (admin
//  messages) behind an explicit confirmation" — "behind an explicit
//  confirmation" applies to all three, not just the channel one).
//
//  Shows EXACTLY what is about to change (the caller's `changes` lines,
//  computed up front by the view model — never re-derived after a
//  write) plus what every admin write here actually does: the node
//  saves, reboots, and disconnects at commit, and THIS APP — not the
//  user — reconnects and reads every changed item back automatically,
//  only reporting success once that read-back matches what was sent
//  (PR #274 review, SHOULD-FIX 6: the old copy asked the user to
//  manually do something `applyChannelSet`/`setOwner`/`setRegion`
//  already do). CONFIRM is disabled while a write is already in
//  flight, so a second tap cannot start a second concurrent one.
//
import SwiftUI

struct AdminWriteConfirmationSheet: View {
    let title: String
    let changes: [String]
    let isBusy: Bool
    let errorMessage: String?
    let onConfirm: () -> Void
    let onCancel: () -> Void

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                Text("This will change:")
                    .font(.system(.caption, design: .monospaced).weight(.bold))
                    .foregroundStyle(Color.ffMuted)
                ForEach(changes, id: \.self) { line in
                    Text(line)
                        .font(.system(.body, design: .monospaced))
                        .foregroundStyle(Color.ffInk)
                }
                Text("The node saves this, then reboots and disconnects. The app reconnects and " +
                     "reads everything back on its own — CONFIRM only succeeds once it matches " +
                     "exactly what you sent.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffAmber)
                if let errorMessage {
                    Text(errorMessage)
                        .font(.footnote)
                        .foregroundStyle(Color.ffAlert)
                }
                Spacer()
                HStack(spacing: 12) {
                    Button("CANCEL", action: onCancel)
                        .buttonStyle(.bordered)
                        .tint(.ffMuted)
                        .disabled(isBusy)
                    Spacer()
                    Button(isBusy ? "APPLYING…" : "CONFIRM", action: onConfirm)
                        .buttonStyle(.borderedProminent)
                        .tint(.ffAmber)
                        .foregroundStyle(Color.ffBackground)
                        .disabled(isBusy)
                }
                .frame(minHeight: 44)
            }
            .padding(20)
            .background(Color.ffBackground)
            .navigationTitle(title)
        }
    }
}
