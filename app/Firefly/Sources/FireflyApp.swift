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
    /// Milestone 1 wires the stub client. Slice 2 replaces this one line
    /// with a transport picker; nothing above this line changes, which
    /// is the point of holding a protocol rather than a class.
    @State private var connect = ConnectViewModel(client: StubMeshtasticClient())

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
