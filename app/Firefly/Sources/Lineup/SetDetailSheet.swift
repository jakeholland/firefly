//
//  SetDetailSheet.swift — tapping a set (a grid block or a picks row)
//  opens this: stage, time, a star button, and a disabled "Message
//  crew" stub (docs/specs/A01-companion-app.md, Lineup: "opens a small
//  detail sheet with a star button; also shows stage, time, and a
//  'Message crew' affordance stub that is disabled for now").
//
import FireflyModel
import SwiftUI

struct SetDetailSheet: View {
    @Bindable var model: LineupViewModel
    let set: FestpackScheduleSet
    @Environment(\.dismiss) private var dismiss

    private var stage: FestpackStage? { model.festpack?.stage(withID: set.stageID) }

    var body: some View {
        VStack(spacing: 20) {
            HStack {
                Spacer()
                Button {
                    dismiss()
                    model.dismissSetDetail()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(Color.ffMuted)
                        .font(.title3)
                }
                .buttonStyle(.plain)
            }

            VStack(spacing: 8) {
                StageSwatch(colorRGB: stage?.colorRGB)
                    .frame(width: 14, height: 14)
                Text(set.artist.uppercased())
                    .font(.system(.title2, design: .rounded).weight(.bold))
                    .foregroundStyle(Color.ffInk)
                    .multilineTextAlignment(.center)
                Text(stage?.name ?? "Unknown stage")
                    .font(.system(.subheadline, design: .rounded).weight(.semibold))
                    .foregroundStyle(stage.map { Color(fireflyHex: $0.colorRGB) } ?? Color.ffMuted)
                Text(timeRangeText)
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(Color.ffMuted)
                if let caveat = endCaveatText {
                    Text(caveat)
                        .font(.caption2)
                        .foregroundStyle(Color.ffStaleAmber)
                        .multilineTextAlignment(.center)
                }
                if !set.note.isEmpty {
                    Text(set.note)
                        .font(.footnote)
                        .foregroundStyle(Color.ffMuted)
                }
            }

            Button {
                model.togglePick(set)
            } label: {
                Label(model.isPicked(set) ? "Picked" : "Pick this set",
                      systemImage: model.isPicked(set) ? "star.fill" : "star")
                    .font(.system(.subheadline, design: .rounded).weight(.bold))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
            }
            .background(model.isPicked(set) ? Color.ffAmber : Color.clear)
            .foregroundStyle(model.isPicked(set) ? Color.black : Color.ffInk)
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(model.isPicked(set) ? Color.clear : Color.ffDim, lineWidth: 1.5))
            .clipShape(RoundedRectangle(cornerRadius: 14))

            // Disabled stub — no crew-messaging integration yet (out of
            // this feature's scope; flagged rather than silently
            // omitted, per the feature's own instructions).
            Button {
            } label: {
                Label("Message crew", systemImage: "bubble.left.and.bubble.right")
                    .font(.system(.subheadline, design: .rounded).weight(.semibold))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
            }
            .disabled(true)
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.ffDim, lineWidth: 1.5))
            .foregroundStyle(Color.ffMuted)
            .clipShape(RoundedRectangle(cornerRadius: 14))
            Text("Coming soon")
                .font(.caption2)
                .foregroundStyle(Color.ffMuted)

            Spacer()
        }
        .padding(20)
        .background(Color.ffBackground)
        .presentationDetents([.medium])
    }

    /// Only a PUBLISHED end is shown as part of the time range. An
    /// inferred one (the grid still sizes the block from it) is shown
    /// separately, labelled — see `endCaveatText` — because "21:00 –
    /// 22:00" reads as a fact the festpack never stated.
    private var timeRangeText: String {
        let start = LineupViewModel.timeText(set.startMinute) ?? "TBD"
        guard let end = model.effectiveEnd(for: set), let endText = LineupViewModel.timeText(end.minute) else {
            return start
        }
        return end.isPublished ? "\(start) – \(endText)" : "\(start) – \(endText)?"
    }

    /// `nil` when the end time is published (nothing to caveat) or when
    /// there is no end at all to show.
    private var endCaveatText: String? {
        guard let end = model.effectiveEnd(for: set) else { return nil }
        switch end.source {
        case .published:
            return nil
        case .nextSetOnStage:
            return "End time not published — assumed to run until the next set on this stage."
        case .defaultLength:
            return "End time not published — shown as a \(LineupTimeInference.defaultSetMinutes)-minute block."
        }
    }
}
