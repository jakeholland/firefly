//
//  LineupPicksView.swift — My picks: starred sets grouped by day,
//  sorted by start, with conflict markers when two picks overlap
//  (docs/specs/A01-companion-app.md, Lineup).
//
import FireflyModel
import SwiftUI

struct LineupPicksView: View {
    @Bindable var model: LineupViewModel

    var body: some View {
        let groups = model.pickedGroups
        if groups.isEmpty {
            emptyState
        } else {
            List {
                ForEach(groups, id: \.night) { group in
                    Section {
                        ForEach(group.rows) { row in
                            PickedRowView(row: row, model: model)
                                .listRowBackground(Color.ffBackground)
                                .contentShape(Rectangle())
                                .onTapGesture { model.selectSet(row.set) }
                        }
                    } header: {
                        Text(model.dayPillLabel(for: group.night).uppercased())
                    }
                }
            }
            .listStyle(.plain)
        }
    }

    private var emptyState: some View {
        VStack(spacing: 8) {
            Image(systemName: "star")
                .font(.largeTitle)
                .foregroundStyle(Color.ffMuted)
            Text("Tap a set in the Grid to pick it — it'll show up here.")
                .font(.footnote)
                .foregroundStyle(Color.ffMuted)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct PickedRowView: View {
    let row: LineupViewModel.PickedRow
    let model: LineupViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 12) {
                Text(LineupViewModel.timeText(row.set.startMinute) ?? "TBD")
                    .font(.system(.subheadline, design: .monospaced))
                    .foregroundStyle(Color.ffMuted)
                    .frame(width: 62, alignment: .leading)
                Rectangle()
                    .fill(row.stage.map { Color(fireflyHex: $0.colorRGB) } ?? Color.ffMuted)
                    .frame(width: 3, height: 40)
                    .clipShape(RoundedRectangle(cornerRadius: 2))
                VStack(alignment: .leading, spacing: 4) {
                    Text(row.set.artist.uppercased())
                        .font(.system(.subheadline, design: .rounded).weight(.bold))
                        .lineLimit(1)
                    HStack(spacing: 8) {
                        Text((row.stage?.name ?? "Unknown stage").uppercased())
                            .font(.system(.caption2, design: .rounded).weight(.semibold))
                            .foregroundStyle(row.stage.map { Color(fireflyHex: $0.colorRGB) } ?? Color.ffMuted)
                        if !row.set.note.isEmpty {
                            Text(row.set.note)
                                .font(.caption2)
                                .foregroundStyle(Color.ffMuted)
                        }
                    }
                }
                Spacer()
                Button {
                    model.togglePick(row.set)
                } label: {
                    Image(systemName: "star.fill")
                        .foregroundStyle(Color.ffAmber)
                }
                .buttonStyle(.plain)
            }
            if !row.conflictsWithArtists.isEmpty {
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(Color.ffStaleAmber)
                    // "may overlap" when the clash was decided from an
                    // end time the pack never published (`PickedRow
                    // .conflictsAreInferred`) — an inferred collision
                    // is not a published one.
                    Text("\(row.conflictsAreInferred ? "may overlap" : "overlaps") \(row.conflictsWithArtists.joined(separator: ", "))")
                        .font(.caption)
                        .foregroundStyle(Color.ffStaleAmber)
                }
                .padding(8)
                .background(Color.ffBackground)
                .clipShape(RoundedRectangle(cornerRadius: 10))
            }
        }
        .padding(.vertical, 4)
    }
}
