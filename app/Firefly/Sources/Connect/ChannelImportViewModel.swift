//
//  ChannelImportViewModel.swift — the Connect screen's channel-import
//  section: paste or scan a `https://meshtastic.org/e/#…` link, show
//  what it decodes to (docs/specs/A01-companion-app.md's M1
//  Connect-screen bullet). M1 never writes it to a node — see
//  ChannelURL.swift's header for the scope cut this follows.
//
import FireflyModel
import Foundation
import Observation

@MainActor
@Observable
final class ChannelImportViewModel {
    private(set) var result: ChannelImportResult?
    private(set) var errorMessage: String?

    func importURL(_ text: String) {
        errorMessage = nil
        do {
            result = try ChannelURL.parse(text)
        } catch {
            result = nil
            errorMessage = Self.message(for: error)
        }
    }

    func clear() {
        result = nil
        errorMessage = nil
    }

    /// The one thing this screen must never say about a channel it
    /// just imported: that its position precision is safe when the URL
    /// never said so. Surfaced explicitly rather than silently patched
    /// — see `ChannelSettings.missingExplicitPositionPrecision`.
    var precisionWarnings: [String] {
        guard let result else { return [] }
        return result.channelSet.settings
            .filter(\.missingExplicitPositionPrecision)
            .map { settings in
                let name = settings.name.isEmpty ? "(default)" : settings.name
                return "\(name): no position-precision limit was in this link — a node that " +
                       "later sends this channel treats that as FULL precision, exact coordinates."
            }
    }

    private static func message(for error: Error) -> String {
        guard let channelError = error as? ChannelURLError else { return String(describing: error) }
        switch channelError {
        case .unsupportedScheme: return "Not a Meshtastic channel link."
        case .missingFragment: return "The link has no channel payload."
        case .malformedBase64: return "The channel payload isn't valid."
        case .malformedProtobuf: return "The channel payload couldn't be read."
        }
    }
}
