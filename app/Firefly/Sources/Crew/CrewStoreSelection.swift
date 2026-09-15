//
//  CrewStoreSelection.swift — REVIEW FIX (PR #331 independent review,
//  BLOCKING): which `CrewController` stores a composition gets, as a
//  pure function of its `AppDependencies` — split out of
//  `AppRuntimeBundle.build` the same reason `DemoModeAction`/
//  `RootLaunchPlan` are their own files (those files' own header
//  comments): so `FireflyAppTests` can pin the demo-isolation rule
//  directly, without `AppRuntimeBundle.swift`'s own `AppGraph`/
//  view-model composition (which that file's own project.yml comment
//  says is exercised by `FireflyUITests` instead, not duplicated here).
//
//  THE RULE: a demo composition (`dependencies.client is
//  DemoMeshtasticClient`) must never be handed the real, persistent
//  `CrewSnapshotKeychainStore`/`CrewHiddenStore` —
//  `docs/specs/A01-companion-app.md`'s "Demo isolation" section, and
//  this PR's own body ("Nothing demo-shaped ever touches UserDefaults,
//  Keychain..."). Before this file existed, `AppRuntimeBundle.build`
//  wired the real stores into `CrewController` UNCONDITIONALLY — for
//  the demo composition too. Harmless while `.demoBundle()` only ever
//  ran inside the iOS Simulator (a disposable Keychain/UserDefaults with
//  no real user data in it); a real leak now that "Try the demo" reaches
//  this exact composition on a REAL DEVICE with the user's real crew
//  data already in the real Keychain/`UserDefaults.standard` — hiding a
//  demo crew member (People list) would have written a real
//  `firefly.crew.hidden.<demo code>.v1` UserDefaults key, and a demo
//  Start/Join would have permanently poisoned the ONE-TIME
//  `saveIfAbsent` pre-crew Keychain snapshot with fabricated demo
//  channel data.
//
//  Same `dependencies.client is DemoMeshtasticClient` test
//  `AppRuntimeBundle.build`'s own `indexProvider` line already uses for
//  the identical "is this composition the demo world?" question.
//
import FireflyMesh
import FireflyModel

enum CrewStoreSelection {
    static func snapshotStore(for dependencies: AppDependencies) -> any CrewSnapshotStoring {
        isDemo(dependencies) ? InMemoryCrewSnapshotStore() : CrewSnapshotKeychainStore()
    }

    static func hiddenStore(for dependencies: AppDependencies) -> any CrewHiddenStoring {
        isDemo(dependencies) ? InMemoryCrewHiddenStore() : CrewHiddenStore()
    }

    private static func isDemo(_ dependencies: AppDependencies) -> Bool {
        dependencies.client is DemoMeshtasticClient
    }
}
