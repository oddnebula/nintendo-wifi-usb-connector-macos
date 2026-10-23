import SwiftUI

@main
struct WFCConnectorApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = ConnectorModel()

    var body: some Scene {
        WindowGroup("Nintendo Wi-Fi USB Connector") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 680, minHeight: 560)
                .onAppear { delegate.model = model }
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var model: ConnectorModel?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        // Stop broadcasting and shut the root helper down with the app.
        MainActor.assumeIsolated { model?.quitHelper() }
        Thread.sleep(forTimeInterval: 0.3)
    }
}
