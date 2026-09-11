//
//  DemoFestpackProvider.swift — demo mode's festival data (owner's
//  instructions: "Demo mode uses the Firefly Fields pack
//  (firmware/assets/demo/firefly-fields.festpack.json) from the bundle
//  only").
//
//  Deliberately never touches the network or the disk cache — demo mode
//  (`-FireflyDemo`, iOS Simulator only, see `DemoLaunch.swift`) must
//  render the SAME scripted world on every run, never something that
//  quietly depends on what a previous `AlmanacFestpackProvider` run
//  happened to cache.
//
import FireflyMesh
import Foundation

public actor DemoFestpackProvider: FestpackProviding {
    /// `nonisolated` — see `AlmanacFestpackProvider`'s identical property
    /// for why this is safe.
    private nonisolated let hub = CurrentValueEventHub<Festpack>()
    private var pack: Festpack?
    private let bundleLoader: any FestpackBundleLoading

    public init(bundleLoader: any FestpackBundleLoading = MainBundleFestpackLoader()) {
        self.bundleLoader = bundleLoader
    }

    public func current() -> Festpack? { pack }
    public func sourceState() -> FestpackSourceState { pack == nil ? .none : .bundled }
    public nonisolated func festpackUpdates() -> AsyncStream<Festpack> { hub.subscribe() }

    /// Loads the bundled Firefly Fields pack exactly once; later calls
    /// are no-ops — there is nothing to "refresh" against, on purpose.
    public func refresh() async {
        guard pack == nil else { return }
        guard let data = bundleLoader.festpackData(forResource: "firefly-fields", extension: "festpack.json"),
              case .success(let parsed) = FestpackParser.parse(data) else { return }
        pack = parsed
        hub.yield(parsed)
    }
}
