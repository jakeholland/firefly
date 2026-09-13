//
//  FestpackSourceURLValidator.swift — the ONE https-only validation
//  rule for whatever ends up in `SettingsKey.festpackSourceURLOverride`
//  (docs/specs/A01-companion-app.md, Festival data).
//
//  Pulled out of `SettingsViewModel` (app target) so BOTH writers of
//  that setting — the Settings screen's manual "Pack URL" field
//  (advanced override) AND `FestivalPickerViewModel`'s picker, which
//  builds a URL itself from an almanac index entry's `path` — reject
//  the exact same way a non-https value would be rejected, rather than
//  the picker trusting an almanac-supplied path unchecked while the
//  manual field alone does the checking. `AlmanacFestpackProvider
//  .sourceURL()` re-checks the same rule on READ, as the last line of
//  defense for a value that reached the store some other way (a synced
//  or corrupted default) — this type is the shared WRITE-time rule the
//  two UI call sites share.
//
import Foundation

/// `Result<String?, String>` cannot express this (`String` does not
/// conform to `Error`, and this is not really an "error" in the
/// throwing sense — a rejection here is an ordinary, expected outcome
/// a caller branches on, not a control-flow exception).
public enum FestpackSourceURLValidation: Sendable, Equatable {
    /// Blank/whitespace-only input — clear the override, use the
    /// built-in default.
    case clear
    /// A trimmed, validated https URL string, ready to store.
    case use(String)
    /// Reject outright; the caller must leave whatever was previously
    /// stored untouched and show this message.
    case reject(String)
}

public enum FestpackSourceURLValidator {
    public static let httpsOnlyError = "Festival data URL must start with https:// — keeping the previous value."

    public static func validate(_ raw: String?) -> FestpackSourceURLValidation {
        let trimmed = raw?.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let trimmed, !trimmed.isEmpty else { return .clear }
        guard let url = URL(string: trimmed), url.scheme?.lowercased() == "https" else {
            return .reject(httpsOnlyError)
        }
        return .use(trimmed)
    }
}
