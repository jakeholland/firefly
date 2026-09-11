//
//  FireflyApp.swift — the multiplatform shell.
//
//  One target, two platforms (iOS 17+, macOS 14+). The Mac build is not
//  a courtesy port: the iOS Simulator has no Bluetooth at all, so the
//  Mac — with its own CoreBluetooth radio and a USB-serial transport —
//  is where this app is tested against a real Heltec.
//  See docs/specs/A01-companion-app.md.
//
import FireflyMesh
import FireflyModel
import SwiftUI

@main
struct FireflyApp: App {
    /// `AppDependencies.current()` is `.stub()` in the iOS Simulator and
    /// `.live()` everywhere else (today, also the stub stack — slice A's
    /// real BLE client and slice F's real location/heading providers
    /// replace it there; see `AppDependencies.live()`'s own comment).
    /// Slice C wires the transport picker on top of this; nothing above
    /// this line changes when it does, which is the point of holding
    /// protocols rather than concrete types.
    let dependencies: AppDependencies
    @State private var connect: ConnectViewModel

    init() {
        let dependencies = AppDependencies.current()
        self.dependencies = dependencies
        _connect = State(initialValue: ConnectViewModel(client: dependencies.client))
    }

    var body: some Scene {
        WindowGroup {
            RootView(connect: connect)
                .preferredColorScheme(.dark)
        }
        #if os(macOS)
        .defaultSize(width: 420, height: 720)
        #endif
    }
}
