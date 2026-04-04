import SwiftUI

@main
struct BE300App: App {
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var engine = EmulatorEngine()

    var body: some Scene {
        WindowGroup {
            EmulatorView(engine: engine)
                .onChange(of: scenePhase) { newPhase in
                    switch newPhase {
                    case .active:
                        if !engine.isRunning {
                            engine.start()
                        }
                    case .background, .inactive:
                        engine.stop()
                    @unknown default:
                        break
                    }
                }
                .onAppear {
                    engine.boot()
                    engine.start()
                }
        }
    }
}
