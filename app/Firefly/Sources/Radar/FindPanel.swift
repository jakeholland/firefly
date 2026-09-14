//
//  FindPanel.swift — S29 PR2's FIND mode: active pings, a replies list,
//  and the WARMER/COLDER headline.
//
import FireflyModel
import SwiftUI

struct FindPanel: View {
    @Bindable var model: RadarViewModel

    var body: some View {
        VStack(spacing: 16) {
            Text("FIND")
                .font(.system(.title2, design: .rounded).weight(.bold))
                .foregroundStyle(Color.ffAmber)

            if model.isFindActive {
                Text(model.findTrendHeadline)
                    .font(.system(.largeTitle, design: .rounded).weight(.heavy))
                    .foregroundStyle(trendColor)

                Text("\(model.findPingCount) pings sent, one every \(Int(FindSessionConstants.pingIntervalSeconds)) s")
                    .font(.footnote)
                    .foregroundStyle(Color.ffCaption)

                if model.findReplies.isEmpty {
                    Text("No replies yet.")
                        .font(.callout)
                        .foregroundStyle(Color.ffCaption)
                } else {
                    // Owner decision, 2026-09-13 ("Radar/Find detail
                    // lines... FIND reply rows: signal word instead of
                    // dBm/SNR"): a plain signal word plus how long ago.
                    // The raw dBm/SNR are dropped from this row — the
                    // tier word IS that reading, bucketed; the numbers
                    // themselves have no Advanced home yet (A02 §6.5
                    // builds one), so nothing here claims they moved.
                    //
                    // PR #304 review: the age is computed from each
                    // reply's own `receivedAt` against a 1 Hz timeline,
                    // never a string captured when the PONG landed — the
                    // second reply of a ten-minute FIND must not still
                    // read "just now" because nothing re-rendered.
                    TimelineView(.periodic(from: .now, by: 1)) { context in
                        List(model.findReplies.reversed()) { reply in
                            HStack {
                                Text(reply.tier.label)
                                    .font(.system(.caption, design: .monospaced).bold())
                                    .foregroundStyle(Color.ffLiveGreen)
                                Spacer()
                                Text(PresenceAge.ago(context.date.timeIntervalSince(reply.receivedAt)))
                                    .font(.system(.caption, design: .monospaced))
                                    .foregroundStyle(Color.ffCaption)
                            }
                            .listRowBackground(Color.radarSurface)
                        }
                        .listStyle(.plain)
                    }
                    .frame(maxHeight: 220)
                }

                Button("STOP") { model.stopFind() }
                    .buttonStyle(.bordered)
                    .tint(.radarStaleAmber)
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
            } else {
                Text("Pings the selected friend every \(Int(FindSessionConstants.pingIntervalSeconds)) s "
                     + "and shows how THEY hear US — up to \(FindSessionConstants.maxPings) pings or "
                     + "\(Int(FindSessionConstants.sessionMaxSeconds / 60)) minutes, whichever comes first.")
                    .font(.footnote)
                    .foregroundStyle(Color.ffCaption)
                    .multilineTextAlignment(.center)

                Button("START FIND") { model.startFindOnSelection() }
                    .buttonStyle(.borderedProminent)
                    .tint(.ffAmber)
                    .foregroundStyle(Color.ffBackground)
                    .frame(minWidth: 44, minHeight: 44)
                    .contentShape(Rectangle())
                    .disabled(model.findTargetNodeID == nil)

                if model.findTargetNodeID == nil {
                    Text("Select a friend first.")
                        .font(.caption)
                        .foregroundStyle(Color.ffCaption)
                }
            }
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.ffBackground)
    }

    private var trendColor: Color {
        switch model.findHaptic {
        case .warmer: return .ffLiveGreen
        case .colder: return .radarStaleAmber
        case .none: return .ffMuted
        }
    }
}
