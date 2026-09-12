//
//  NetworkConnectivityMonitor.swift — Map tab slice: the real,
//  `NWPathMonitor`-backed `NetworkConnectivityObserving` (see
//  `MapViewModel.swift` for the protocol and why it exists — this is
//  the slice-equivalent of `LocationProvider.swift` filling
//  `LocationProviding` in for real on both platforms).
//
//  This ONLY answers "is there a network path right now" — it says
//  nothing about whether Apple Maps' own tiles are cached (see
//  `MapViewModel.offlineChipText`'s doc comment for why this app makes
//  no claim about that at all).
//
import Foundation
import Network

public final class NetworkConnectivityMonitor: NetworkConnectivityObserving, @unchecked Sendable {
    public init() {}

    public func connectivityUpdates() -> AsyncStream<MapConnectivity> {
        AsyncStream { continuation in
            let monitor = NWPathMonitor()
            let queue = DispatchQueue(label: "com.jakeholland.firefly.map.connectivity")
            monitor.pathUpdateHandler = { path in
                continuation.yield(path.status == .satisfied ? .online : .offline)
            }
            monitor.start(queue: queue)
            continuation.onTermination = { _ in monitor.cancel() }
        }
    }
}
