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
                    .foregroundStyle(Color.ffMuted)

                if model.findReplies.isEmpty {
                    Text("No replies yet.")
                        .font(.callout)
                        .foregroundStyle(Color.ffMuted)
                } else {
                    List(model.findReplies.reversed()) { reply in
                        HStack {
                            Text(reply.tier.label)
                                .font(.system(.caption, design: .monospaced).bold())
                                .foregroundStyle(Color.ffLiveGreen)
                            Spacer()
                            Text("\(reply.rssiOfUs) dBm")
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(Color.ffInk)
                            if reply.hasSNR {
                                Text(String(format: "SNR %.1f dB", reply.snrOfUs))
                                    .font(.system(.caption2, design: .monospaced))
                                    .foregroundStyle(Color.ffMuted)
                            }
                        }
                        .listRowBackground(Color.radarSurface)
                    }
                    .listStyle(.plain)
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
                    .foregroundStyle(Color.ffMuted)
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
                        .foregroundStyle(Color.ffMuted)
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
