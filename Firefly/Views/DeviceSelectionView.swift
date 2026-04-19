//
//  DeviceSelectionView.swift
//  Firefly
//
//  Created by Jake Holland on 2/21/26.
//

import SwiftUI

/// View for scanning and selecting a Meshtastic device to connect to
struct DeviceSelectionView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var viewModel: InboxViewModel

    var body: some View {
#if os(macOS)
        macOSLayout
#else
        iOSLayout
#endif
    }

    // MARK: - iOS layout (full-screen sheet with NavigationStack)

#if !os(macOS)
    private var iOSLayout: some View {
        NavigationStack {
            ZStack {
                LinearGradient(
                    colors: [.black, Color(red: 0.1, green: 0, blue: 0.2)],
                    startPoint: .top,
                    endPoint: .bottom
                )
                .ignoresSafeArea()

                VStack(spacing: 0) {
                    if viewModel.discoveredDevices.isEmpty {
                        emptyStateView
                    } else {
                        deviceList
                    }
                }
            }
            .navigationTitle("Select Device")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        viewModel.stopScanning()
                        dismiss()
                    }
                }
            }
            .onAppear { viewModel.startScanning() }
            .onDisappear { viewModel.stopScanning() }
        }
        .preferredColorScheme(.dark)
    }
#endif

    // MARK: - macOS layout (compact popover-style panel)

#if os(macOS)
    private var macOSLayout: some View {
        ZStack {
            LinearGradient(
                colors: [.black, Color(red: 0.1, green: 0, blue: 0.2)],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()

            VStack(spacing: 0) {
                // Title bar
                HStack {
                    Text("Select Device")
                        .font(.headline)
                        .foregroundStyle(.primary)

                    Spacer()

                    Button("Cancel") {
                        viewModel.stopScanning()
                        dismiss()
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(.secondary)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 14)
                .background(Color.black.opacity(0.4))

                Divider()
                    .overlay(Color.white.opacity(0.1))

                if viewModel.discoveredDevices.isEmpty {
                    emptyStateView
                } else {
                    deviceList
                }
            }
        }
        .frame(minWidth: 380, minHeight: 260)
        .preferredColorScheme(.dark)
        .onAppear { viewModel.startScanning() }
        .onDisappear { viewModel.stopScanning() }
    }
#endif

    // MARK: - Shared sub-views

    private var deviceList: some View {
        List {
            Section {
                ForEach(viewModel.discoveredDevices) { device in
                    DeviceRowView(device: device) {
                        viewModel.connectToDevice(device)
                        dismiss()
                    }
                    .listRowBackground(Color.black.opacity(0.3))
                }
            } header: {
                Text("Available Devices")
                    .foregroundStyle(.secondary)
            }
        }
#if os(iOS)
        .listStyle(.insetGrouped)
#else
        .listStyle(.inset)
#endif
        .scrollContentBackground(.hidden)
    }

    private var emptyStateView: some View {
        VStack(spacing: 16) {
            ProgressView()
                .scaleEffect(1.5)
                .tint(.cyan)
                .padding(.bottom, 4)

            Text("Scanning for devices...")
                .font(.title3)
                .foregroundStyle(.secondary)

            Text("Make sure your Meshtastic device is powered on and nearby.\nOnly one BLE connection is supported at a time.")
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(.vertical, 32)
    }
}

struct DeviceRowView: View {
    let device: PeripheralDevice
    let onSelect: () -> Void

    var body: some View {
        Button(action: onSelect) {
            HStack(spacing: 16) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .font(.title2)
                    .foregroundStyle(.cyan)
                    .frame(width: 40, height: 40)
                    .background(.cyan.opacity(0.2))
                    .clipShape(Circle())

                VStack(alignment: .leading, spacing: 4) {
                    Text(device.name)
                        .font(.headline)
                        .foregroundStyle(.primary)

                    HStack(spacing: 8) {
                        signalStrengthIndicator

                        Text(device.signalStrength)
                            .font(.caption)
                            .foregroundStyle(.secondary)

                        Text("•")
                            .foregroundStyle(.tertiary)

                        Text("\(device.rssi) dBm")
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                }

                Spacer()

                Image(systemName: "chevron.right")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            .padding(.vertical, 8)
        }
        .buttonStyle(.plain)
    }

    private var signalStrengthIndicator: some View {
        HStack(spacing: 2) {
            ForEach(0..<4) { index in
                RoundedRectangle(cornerRadius: 1)
                    .fill(signalColor(for: index))
                    .frame(width: 3, height: CGFloat(4 + index * 2))
            }
        }
    }

    private func signalColor(for index: Int) -> Color {
        let strength = device.rssi
        if strength > -50 && index < 4 { return .green }
        else if strength > -70 && index < 3 { return .yellow }
        else if strength > -85 && index < 2 { return .orange }
        else if index < 1 { return .red }
        else { return .gray.opacity(0.3) }
    }
}
