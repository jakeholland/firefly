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
            .onAppear {
                viewModel.startScanning()
            }
            .onDisappear {
                viewModel.stopScanning()
            }
        }
        .preferredColorScheme(.dark)
    }
    
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
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
    }
    
    private var emptyStateView: some View {
        VStack(spacing: 20) {
            ProgressView()
                .scaleEffect(1.5)
                .tint(.cyan)
            
            Text("Scanning for devices...")
                .font(.title3)
                .foregroundStyle(.secondary)
            
            Text("Make sure your Meshtastic device is powered on and nearby")
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
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
