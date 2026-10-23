import SwiftUI

enum Tab: String, CaseIterable, Identifiable {
    case consoles = "Consoles", setup = "Setup", log = "Log"
    var id: String { rawValue }
    var icon: String {
        switch self {
        case .consoles: return "person.2.wave.2"
        case .setup: return "gearshape"
        case .log: return "list.bullet.rectangle"
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: ConnectorModel
    @StateObject private var tab = Box(Tab.consoles)

    var body: some View {
        VStack(spacing: 0) {
            HeaderView()
            Picker("", selection: $tab.value) {
                ForEach(Tab.allCases) { t in Label(t.rawValue, systemImage: t.icon).tag(t) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .padding(.horizontal, 20)
            .padding(.vertical, 10)
            .background(Theme.wfcSky.opacity(0.5))

            Group {
                switch tab.value {
                case .consoles: ConsolesView()
                case .setup: SetupView()
                case .log: LogView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .overlay {
            if !model.helperConnected { ConnectOverlay() }
        }
        .sheet(item: $model.permissionRequest) { c in PermissionSheet(console: c) }
        .alert("WFC Connector", isPresented: Binding(get: { model.errorMessage != nil },
                                                     set: { if !$0 { model.errorMessage = nil } })) {
            Button("OK") { model.errorMessage = nil }
        } message: {
            Text(model.errorMessage ?? "")
        }
        .onAppear { model.connectOrLaunch() }
    }
}

// MARK: - Header

struct HeaderView: View {
    @EnvironmentObject var model: ConnectorModel

    var body: some View {
        HStack(spacing: 14) {
            ZStack {
                Circle().fill(.white).frame(width: 46, height: 46)
                Image(systemName: "wifi").font(.system(size: 22, weight: .bold)).foregroundStyle(Theme.wfcBlue)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text("Wi-Fi USB Connector").font(.title2.weight(.bold))
                Text(adapterLine).font(.callout).opacity(0.9)
            }
            .foregroundStyle(.white)
            Spacer()
            Button(action: { model.running ? model.stop() : model.start() }) {
                Label(model.running ? "Stop" : "Start", systemImage: model.running ? "stop.fill" : "play.fill")
                    .font(.headline)
                    .padding(.horizontal, 18)
                    .padding(.vertical, 8)
                    .background(.white, in: Capsule())
                    .foregroundStyle(model.running ? Theme.bad : Theme.wfcDeep)
            }
            .buttonStyle(.plain)
            .disabled(!model.adapterPresent || !model.helperConnected)
            .opacity(model.adapterPresent ? 1 : 0.5)
            .keyboardShortcut(.return, modifiers: .command)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 16)
        .background(Theme.header)
    }

    var adapterLine: String {
        if !model.helperConnected { return "Helper not running" }
        if !model.adapterPresent { return "Plug in the Nintendo Wi-Fi USB Connector" }
        return model.running ? "\(model.adapterName) · broadcasting" : "\(model.adapterName) · ready"
    }
}

// MARK: - Consoles

struct ConsolesView: View {
    @EnvironmentObject var model: ConnectorModel

    var registeredOffline: [String] {
        model.allowList.filter { mac in !model.consoles.contains { $0.mac == mac } }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                StatusCard()

                Text("Connected consoles").font(.headline)
                if model.consoles.isEmpty {
                    Card {
                        HStack(spacing: 14) {
                            DSGlyph(color: .secondary.opacity(0.4)).frame(height: 44)
                            VStack(alignment: .leading, spacing: 4) {
                                Text(model.running ? "Waiting for a console…" : "No consoles connected")
                                    .font(.body.weight(.medium))
                                Text(model.running
                                     ? "On your DS open Nintendo WFC Settings → Nintendo Wi-Fi Connection Settings → Connection 1 → Search for an Access Point, and pick “\(model.ssid)”."
                                     : "Press Start to begin broadcasting the connector's network.")
                                    .font(.callout).foregroundStyle(.secondary)
                            }
                        }
                    }
                } else {
                    ForEach(model.consoles) { c in ConsoleRow(console: c) }
                }

                if !registeredOffline.isEmpty {
                    Text("Registered consoles").font(.headline).padding(.top, 4)
                    Card {
                        VStack(spacing: 8) {
                            ForEach(registeredOffline, id: \.self) { mac in
                                HStack {
                                    DSGlyph(color: Theme.ok.opacity(0.7)).frame(height: 24)
                                    Text(model.name(for: mac))
                                    Text(mac).font(.caption.monospaced()).foregroundStyle(.secondary)
                                    Spacer()
                                    Button("Remove") { model.forget(mac) }
                                }
                            }
                        }
                    }
                }

                if !model.denyList.isEmpty {
                    Text("Blocked").font(.headline).padding(.top, 4)
                    Card {
                        VStack(spacing: 8) {
                            ForEach(model.denyList, id: \.self) { mac in
                                HStack {
                                    Image(systemName: "nosign").foregroundStyle(Theme.bad)
                                    Text(model.name(for: mac))
                                    Text(mac).font(.caption.monospaced()).foregroundStyle(.secondary)
                                    Spacer()
                                    Button("Unblock") { model.forget(mac) }
                                }
                            }
                        }
                    }
                }
            }
            .padding(20)
        }
    }
}

struct StatusCard: View {
    @EnvironmentObject var model: ConnectorModel

    var dnsText: String {
        switch model.dnsPreset {
        case .none: return "Mac default"
        case .wiimmfi: return "Wiimmfi (\(model.dns1))"
        case .custom: return model.dns2.isEmpty ? model.dns1 : "\(model.dns1) / \(model.dns2)"
        }
    }

    var body: some View {
        Card {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: model.running ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash")
                    .font(.system(size: 28))
                    .foregroundStyle(model.running ? Theme.wfcBlue : .secondary)
                    .frame(width: 36)
                VStack(alignment: .leading, spacing: 6) {
                    Text(model.running ? "Broadcasting “\(model.ssid)” on channel \(model.channel)" : "Not broadcasting")
                        .font(.headline)
                    Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 3) {
                        GridRow { Text("Online service").foregroundStyle(.secondary); Text(dnsText) }
                        if model.running {
                            GridRow { Text("Console addresses").foregroundStyle(.secondary); Text(model.networkInfo) }
                            GridRow { Text("DNS lookups redirected").foregroundStyle(.secondary); Text("\(model.dnsRedirected)") }
                        }
                        GridRow {
                            Text("New consoles").foregroundStyle(.secondary)
                            Text(model.autoAccept ? "Accepted automatically" : "Ask before granting access")
                        }
                    }
                    .font(.callout)
                }
            }
        }
    }
}

struct ConsoleRow: View {
    @EnvironmentObject var model: ConnectorModel
    let console: Console
    @StateObject private var editing = Box(false)
    @StateObject private var draft = Box("")

    var badge: (String, Color) {
        switch console.policy {
        case .pending: return ("Waiting for permission", Theme.waiting)
        case .allowed: return (console.ip.isEmpty ? "Connecting…" : "Connected", Theme.ok)
        case .denied: return ("Denied", Theme.bad)
        }
    }

    var body: some View {
        Card {
            HStack(spacing: 14) {
                DSGlyph(color: console.policy == .allowed ? Theme.wfcBlue : Theme.waiting).frame(height: 46)
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 8) {
                        if editing.value {
                            TextField("Name", text: $draft.value, onCommit: {
                                model.setName(draft.value, for: console.mac)
                                editing.value = false
                            })
                            .textFieldStyle(.roundedBorder)
                            .frame(maxWidth: 180)
                        } else {
                            Text(model.name(for: console.mac)).font(.headline)
                                .onTapGesture(count: 2) { draft.value = model.name(for: console.mac); editing.value = true }
                                .help("Double-click to rename")
                        }
                        StatusBadge(text: badge.0, color: badge.1)
                        if console.dozing { StatusBadge(text: "Power saving", color: .secondary) }
                    }
                    HStack(spacing: 12) {
                        Text(console.mac).font(.caption.monospaced())
                        if !console.ip.isEmpty { Text(console.ip).font(.caption.monospaced()) }
                        Text("↓\(fmt(console.tx)) ↑\(fmt(console.rx))").font(.caption)
                    }
                    .foregroundStyle(.secondary)
                }
                Spacer()
                SignalBars(rssi: console.rssi)
                if console.policy == .pending {
                    Button("Deny") { model.deny(console.mac) }
                    Button("Grant Access") { model.grant(console.mac) }.buttonStyle(.borderedProminent).tint(Theme.wfcBlue)
                } else {
                    Menu {
                        Button("Rename…") { draft.value = model.name(for: console.mac); editing.value = true }
                        Button("Disconnect") { model.disconnect(console.mac) }
                        Divider()
                        Button("Block", role: .destructive) { model.deny(console.mac) }
                    } label: { Image(systemName: "ellipsis.circle") }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                }
            }
        }
    }

    func fmt(_ b: UInt64) -> String { ByteCountFormatter.string(fromByteCount: Int64(b), countStyle: .file) }
}

// MARK: - Permission prompt (the original's "grant access" dialog)

struct PermissionSheet: View {
    @EnvironmentObject var model: ConnectorModel
    let console: Console

    var body: some View {
        VStack(spacing: 16) {
            DSGlyph().frame(height: 80)
            Text("A console wants to connect").font(.title2.weight(.bold))
            Text("**\(model.name(for: console.mac))** (\(console.mac)) is trying to connect through your Nintendo Wi-Fi USB Connector. Grant access only to consoles you own.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 360)
            HStack(spacing: 12) {
                Button("Deny") { model.deny(console.mac) }
                    .keyboardShortcut(.cancelAction)
                Button("Grant Access") { model.grant(console.mac) }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .tint(Theme.wfcBlue)
            }
            Button("Decide later") { model.permissionRequest = nil }
                .buttonStyle(.link)
                .font(.callout)
        }
        .padding(28)
        .frame(width: 440)
    }
}

// MARK: - Helper overlay

struct ConnectOverlay: View {
    @EnvironmentObject var model: ConnectorModel
    var body: some View {
        ZStack {
            Rectangle().fill(.regularMaterial)
            VStack(spacing: 14) {
                DSGlyph().frame(height: 70)
                Text("Start the connector helper").font(.title2.weight(.bold))
                Text(model.demoMode
                     ? "Demo mode: the helper runs without hardware and simulates two consoles."
                     : "Driving the USB connector and sharing your internet connection needs administrator access. macOS will ask for your password.")
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: 380)
                if model.launching {
                    ProgressView().controlSize(.small)
                } else {
                    Button("Start Helper") { model.connectOrLaunch() }
                        .buttonStyle(.borderedProminent)
                        .tint(Theme.wfcBlue)
                        .controlSize(.large)
                }
                Toggle("Demo mode (no hardware)", isOn: $model.demoMode)
                    .toggleStyle(.checkbox)
                    .font(.callout)
            }
            .padding(30)
        }
    }
}

// MARK: - Log

struct LogView: View {
    @EnvironmentObject var model: ConnectorModel
    static let time: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                List(model.log) { line in
                    HStack(alignment: .top, spacing: 8) {
                        Text(Self.time.string(from: line.date)).foregroundStyle(.secondary)
                        Text(line.text).foregroundStyle(color(line.level)).textSelection(.enabled)
                    }
                    .font(.system(.caption, design: .monospaced))
                    .id(line.id)
                }
                .onChange(of: model.log.count) {
                    if let last = model.log.last { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }
            HStack {
                Text("Helper log: \(DaemonClient.logPath)").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Copy") {
                    let text = model.log.map { "\(Self.time.string(from: $0.date)) \($0.text)" }.joined(separator: "\n")
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(text, forType: .string)
                }
                Button("Clear") { model.log.removeAll() }
            }
            .padding(10)
        }
    }

    func color(_ level: String) -> Color {
        switch level {
        case "error": return Theme.bad
        case "warn": return Theme.waiting
        default: return .primary
        }
    }
}
