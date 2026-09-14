//
//  RegionGateView.swift — §1.7's one-step blocking screen, shared by
//  Start and Join: "Your puck needs to know where you are." The picker
//  is PREFILLED from `Locale.current.region`, never auto-applied — the
//  user confirms it with a tap, going through the existing `setRegion`
//  path (`CrewController.confirmRegion()`).
//
import FireflyMesh
import MeshtasticProto
import SwiftUI

struct RegionGateView: View {
    let controller: CrewController
    let onSaved: () -> Void

    private static let allRegions: [Config.LoRaConfig.RegionCode] = Config.LoRaConfig.RegionCode.allCases
        .filter { $0 != .unset }

    var body: some View {
        VStack(spacing: 20) {
            Spacer()
            Text("Your puck needs to know where you are")
                .font(.title2.weight(.bold))
                .foregroundStyle(Color.ffInk)
                .multilineTextAlignment(.center)
            Text("Radios use different frequencies in different countries. Pick yours once and " +
                 "your puck remembers it.")
                .font(.body)
                .foregroundStyle(Color.ffMuted)
                .multilineTextAlignment(.center)

            Picker("Region", selection: Bindable(controller).regionSelection) {
                ForEach(Self.allRegions, id: \.self) { region in
                    Text(String(describing: region).uppercased()).tag(region)
                }
            }
            .pickerStyle(.menu)
            .tint(Color.ffAmber)

            if let error = controller.regionErrorMessage {
                Text(error).font(.footnote).foregroundStyle(Color.ffAlert)
            }

            Button {
                Task {
                    if await controller.confirmRegion() { onSaved() }
                }
            } label: {
                Text(controller.isSettingRegion ? "SAVING…" : "SAVE AND CARRY ON")
                    .font(.headline)
                    .frame(maxWidth: .infinity, minHeight: 48)
            }
            .buttonStyle(.borderedProminent)
            .tint(Color.ffAmber)
            .foregroundStyle(Color.ffBackground)
            .disabled(controller.isSettingRegion)
            Spacer()
        }
        .padding(24)
        .background(Color.ffBackground)
    }
}
