//
//  AdminWriteConfirmationSheet.swift — M3's one confirmation surface,
//  shared by every admin write this app makes: the Connect screen's
//  "Apply to node" (channel/LoRa) and Settings' node-name/region writes
//  (docs/specs/A01-companion-app.md, M3: "Channel write-back (admin
//  messages) behind an explicit confirmation" — "behind an explicit
//  confirmation" applies to all three, not just the channel one).
//
//  Owner decision, 2026-09-13 ("Apply-to-radio confirmation sheet"):
//  the primary thing shown is now ONE plain sentence (`primaryText`,
//  computed by the caller) — what this write actually does to the
//  person's crew/name/history, in words they'd use themselves — with
//  every Meshtastic-engineering detail (channel index, precision bits,
//  region, modem preset) moved behind a "Technical details" disclosure
//  that stays collapsed until tapped. `changes` still exists for the
//  handful of callers whose lines are ALREADY plain (Settings' name/
//  region summaries: "Long name -> \"X\"") — those render directly,
//  visible, no disclosure needed; `technicalDetails` is for the
//  jargon-heavy ones (the channel-apply sheet's index/precision/preset
//  lines) that should never be the first thing a reader has to parse.
//
//  What every admin write here actually does — the radio saves,
//  restarts, and reconnects on its own — is now `primaryText`'s job to
//  say, in plain words ("Your puck will blink off for a few seconds
//  while it saves this, then reconnect on its own"), not a second,
//  separate paragraph repeating "reboots and disconnects" (PR #274
//  review, SHOULD-FIX 6's original point stands: the user does nothing
//  here but read and confirm — `applyChannelSet`/`setOwner`/
//  `setRegion` do the rest). CONFIRM is disabled while a write is
//  already in flight, so a second tap cannot start a second concurrent
//  one.
//
import SwiftUI

struct AdminWriteConfirmationSheet: View {
    let title: String
    /// One plain sentence naming what this write actually does — the
    /// only thing shown above the fold. Every caller composes this
    /// itself (never derived here), so a "Clear history" sheet — which
    /// touches no radio at all — never claims one restarts.
    let primaryText: String
    /// Plain, already-human-readable lines shown directly beneath
    /// `primaryText`, no disclosure — empty shows nothing here.
    var changes: [String] = []
    /// Meshtastic-engineering lines (channel index, precision bits,
    /// region, modem preset) shown only once "Technical details" is
    /// tapped open — empty shows no disclosure at all.
    var technicalDetails: [String] = []
    let isBusy: Bool
    let errorMessage: String?
    let onConfirm: () -> Void
    let onCancel: () -> Void

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                Text(primaryText)
                    .font(.body)
                    .foregroundStyle(Color.ffInk)
                if !changes.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(changes, id: \.self) { line in
                            Text(line)
                                .font(.system(.body, design: .monospaced))
                                .foregroundStyle(Color.ffInk)
                        }
                    }
                }
                if !technicalDetails.isEmpty {
                    DisclosureGroup("Technical details") {
                        VStack(alignment: .leading, spacing: 6) {
                            ForEach(technicalDetails, id: \.self) { line in
                                Text(line)
                                    .font(.system(.footnote, design: .monospaced))
                                    .foregroundStyle(Color.ffCaption)
                            }
                        }
                        .padding(.top, 6)
                    }
                    .font(.system(.footnote, design: .rounded).weight(.semibold))
                    .tint(.ffCaption)
                }
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
