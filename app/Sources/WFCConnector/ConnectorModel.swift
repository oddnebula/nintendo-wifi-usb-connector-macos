import Foundation
import SwiftUI

struct Console: Identifiable, Equatable {
    enum Policy: String { case pending, allowed, denied }
    let mac: String
    var policy: Policy
    var ip: String
    var rssi: Int
    var rx: UInt64
    var tx: UInt64
    var dozing: Bool
    var id: String { mac }
}

struct LogLine: Identifiable {
    let id = UUID()
    let date = Date()
    let level: String
    let text: String
}

enum DNSPreset: String, CaseIterable, Identifiable {
    case wiimmfi, custom, none
    var id: String { rawValue }
    var title: String {
        switch self {
        case .wiimmfi: return "Wiimmfi"
        case .custom: return "Custom"
        case .none: return "Mac default (no redirect)"
        }
    }
    /// Wiimmfi's DNS server, as listed by the DS homebrew wiki. Editable in the UI because
    /// community servers occasionally move.
    static let wiimmfiDNS = "178.62.43.212"
}

@MainActor
final class ConnectorModel: ObservableObject {
    // Daemon / adapter state
    @Published var helperConnected = false
    @Published var launching = false
    @Published var adapterPresent = false
    @Published var adapterName = ""
    @Published var adapterMAC = ""
    @Published var adapterRF = ""
    @Published var running = false
    @Published var networkInfo = ""
    @Published var dnsRedirected: UInt64 = 0
    @Published var consoles: [Console] = []
    @Published var log: [LogLine] = []
    @Published var errorMessage: String?
    @Published var permissionRequest: Console?

    // Persisted settings
    private static let d = UserDefaults.standard
    @Published var ssid = d.string(forKey: "ssid") ?? "NintendoWFC" { didSet { Self.d.set(ssid, forKey: "ssid") } }
    @Published var channel = d.object(forKey: "channel") as? Int ?? 6 { didSet { Self.d.set(channel, forKey: "channel") } }
    @Published var autoAccept = d.bool(forKey: "autoAccept") { didSet { Self.d.set(autoAccept, forKey: "autoAccept") } }
    @Published var dnsPresetRaw = d.string(forKey: "dnsPreset") ?? DNSPreset.wiimmfi.rawValue { didSet { Self.d.set(dnsPresetRaw, forKey: "dnsPreset") } }
    @Published var dns1 = d.string(forKey: "dns1") ?? DNSPreset.wiimmfiDNS { didSet { Self.d.set(dns1, forKey: "dns1") } }
    @Published var dns2 = d.string(forKey: "dns2") ?? "" { didSet { Self.d.set(dns2, forKey: "dns2") } }
    @Published var demoMode = d.bool(forKey: "demoMode") { didSet { Self.d.set(demoMode, forKey: "demoMode") } }
    @Published private var allowListRaw = d.string(forKey: "allowList") ?? "[]" { didSet { Self.d.set(allowListRaw, forKey: "allowList") } }
    @Published private var denyListRaw = d.string(forKey: "denyList") ?? "[]" { didSet { Self.d.set(denyListRaw, forKey: "denyList") } }
    @Published private var nicknamesRaw = d.string(forKey: "nicknames") ?? "{}" { didSet { Self.d.set(nicknamesRaw, forKey: "nicknames") } }

    private let client = DaemonClient()
    private var askedAbout = Set<String>()

    var dnsPreset: DNSPreset {
        get { DNSPreset(rawValue: dnsPresetRaw) ?? .wiimmfi }
        set {
            dnsPresetRaw = newValue.rawValue
            switch newValue {
            case .wiimmfi: dns1 = DNSPreset.wiimmfiDNS; dns2 = ""
            case .none: dns1 = ""; dns2 = ""
            case .custom: break
            }
        }
    }

    var allowList: [String] {
        get { decode(allowListRaw) ?? [] }
        set { allowListRaw = encode(newValue) }
    }
    var denyList: [String] {
        get { decode(denyListRaw) ?? [] }
        set { denyListRaw = encode(newValue) }
    }
    private var nicknames: [String: String] {
        get { decode(nicknamesRaw) ?? [:] }
        set { nicknamesRaw = encode(newValue) }
    }

    init() {
        client.onEvent = { [weak self] ev in
            Task { @MainActor in self?.handle(ev) }
        }
        client.onDisconnect = { [weak self] in
            Task { @MainActor in self?.helperLost() }
        }
    }

    // MARK: - Helper lifecycle

    func connectOrLaunch() {
        if client.isConnected { return }
        if client.connect() { helperConnected = true; return }
        launching = true
        let simulate = demoMode
        Task.detached { [client] in
            do {
                try client.launchHelper(simulate: simulate)
                var ok = false
                for _ in 0..<40 {
                    try? await Task.sleep(nanoseconds: 150_000_000)
                    if client.connect() { ok = true; break }
                }
                let connected = ok
                await MainActor.run {
                    self.launching = false
                    self.helperConnected = connected
                    if !connected { self.errorMessage = "The connector helper did not start. See \(DaemonClient.logPath)." }
                }
            } catch {
                await MainActor.run {
                    self.launching = false
                    self.errorMessage = error.localizedDescription
                }
            }
        }
    }

    func quitHelper() {
        client.send(["cmd": "quit"])
    }

    /// Restarts the helper, e.g. after switching demo mode.
    func relaunchHelper() {
        client.send(["cmd": "quit"])
        client.disconnect()
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { self.connectOrLaunch() }
    }

    private func helperLost() {
        helperConnected = false
        running = false
        adapterPresent = false
        consoles = []
    }

    // MARK: - Commands

    func start() {
        guard validDNS(dns1, allowEmpty: dnsPreset == .none), validDNS(dns2, allowEmpty: true) else {
            errorMessage = "Enter a valid IPv4 address for the DNS server (for Wiimmfi: \(DNSPreset.wiimmfiDNS))."
            return
        }
        client.send([
            "cmd": "start", "ssid": ssid, "channel": channel,
            "dns1": dnsPreset == .none ? "" : dns1, "dns2": dnsPreset == .none ? "" : dns2,
            "auto_accept": autoAccept, "allow": allowList, "deny": denyList,
        ])
    }

    func stop() { client.send(["cmd": "stop"]) }

    func applyDNS() {
        guard validDNS(dns1, allowEmpty: dnsPreset == .none), validDNS(dns2, allowEmpty: true) else {
            errorMessage = "Enter a valid IPv4 address for the DNS server."
            return
        }
        client.send(["cmd": "set_dns", "dns1": dnsPreset == .none ? "" : dns1,
                     "dns2": dnsPreset == .none ? "" : dns2])
    }

    func setAutoAccept(_ on: Bool) {
        autoAccept = on
        client.send(["cmd": "set_auto_accept", "on": on])
    }

    func grant(_ mac: String) {
        allowList = Array(Set(allowList + [mac])).sorted()
        denyList = denyList.filter { $0 != mac }
        client.send(["cmd": "allow", "mac": mac])
        if permissionRequest?.mac == mac { permissionRequest = nil }
    }

    func deny(_ mac: String) {
        denyList = Array(Set(denyList + [mac])).sorted()
        allowList = allowList.filter { $0 != mac }
        client.send(["cmd": "deny", "mac": mac])
        if permissionRequest?.mac == mac { permissionRequest = nil }
    }

    func disconnect(_ mac: String) { client.send(["cmd": "kick", "mac": mac]) }

    func forget(_ mac: String) {
        allowList = allowList.filter { $0 != mac }
        denyList = denyList.filter { $0 != mac }
        askedAbout.remove(mac)
        client.send(["cmd": "forget", "mac": mac])
    }

    // MARK: - Names

    func name(for mac: String) -> String {
        if let n = nicknames[mac], !n.isEmpty { return n }
        return NintendoOUI.isNintendo(mac) ? "Nintendo DS" : "Unknown device"
    }

    func setName(_ name: String, for mac: String) {
        var n = nicknames
        n[mac] = name.trimmingCharacters(in: .whitespaces)
        nicknames = n
    }

    // MARK: - Events

    private func handle(_ ev: [String: Any]) {
        switch ev["event"] as? String {
        case "adapter":
            adapterPresent = ev["present"] as? Bool ?? false
            adapterName = ev["name"] as? String ?? ""
            adapterMAC = ev["mac"] as? String ?? ""
            adapterRF = ev["rf"] as? String ?? ""
        case "state":
            running = ev["running"] as? Bool ?? false
            networkInfo = ev["network"] as? String ?? ""
            dnsRedirected = (ev["dns_redirected"] as? NSNumber)?.uint64Value ?? 0
            if !running { consoles = [] }
        case "station":
            guard let mac = ev["mac"] as? String else { return }
            let c = Console(mac: mac,
                            policy: Console.Policy(rawValue: ev["policy"] as? String ?? "") ?? .pending,
                            ip: ev["ip"] as? String ?? "",
                            rssi: ev["rssi"] as? Int ?? 0,
                            rx: (ev["rx"] as? NSNumber)?.uint64Value ?? 0,
                            tx: (ev["tx"] as? NSNumber)?.uint64Value ?? 0,
                            dozing: ev["ps"] as? Bool ?? false)
            if let i = consoles.firstIndex(where: { $0.mac == mac }) { consoles[i] = c } else { consoles.append(c) }
            if c.policy == .pending && !askedAbout.contains(mac) && permissionRequest == nil {
                askedAbout.insert(mac)
                permissionRequest = c
                NSApp.requestUserAttention(.criticalRequest)
            }
        case "station_gone":
            guard let mac = ev["mac"] as? String else { return }
            consoles.removeAll { $0.mac == mac }
            if permissionRequest?.mac == mac { permissionRequest = nil }
            askedAbout.remove(mac)
        case "log":
            append(level: ev["level"] as? String ?? "info", ev["msg"] as? String ?? "")
        case "error":
            let msg = ev["msg"] as? String ?? "Unknown error"
            append(level: "error", msg)
            errorMessage = msg
        default:
            break
        }
    }

    private func append(level: String, _ text: String) {
        log.append(LogLine(level: level, text: text))
        if log.count > 1000 { log.removeFirst(log.count - 1000) }
    }

    // MARK: - Helpers

    func validDNS(_ s: String, allowEmpty: Bool) -> Bool {
        let t = s.trimmingCharacters(in: .whitespaces)
        if t.isEmpty { return allowEmpty }
        var addr = in_addr()
        return inet_pton(AF_INET, t, &addr) == 1
    }

    private func decode<T: Decodable>(_ s: String) -> T? {
        try? JSONDecoder().decode(T.self, from: Data(s.utf8))
    }
    private func encode<T: Encodable>(_ v: T) -> String {
        (try? String(data: JSONEncoder().encode(v), encoding: .utf8)) ?? ""
    }
}

enum NintendoOUI {
    /// IEEE OUIs registered to Nintendo Co., Ltd. that appear on DS / DS Lite / DSi consoles.
    static let prefixes: Set<String> = [
        "00:09:bf", "00:16:56", "00:17:ab", "00:19:1d", "00:19:fd", "00:1a:e9", "00:1b:7a",
        "00:1b:ea", "00:1c:be", "00:1d:bc", "00:1e:35", "00:1e:a9", "00:1f:32", "00:1f:c5",
        "00:21:47", "00:21:bd", "00:22:4c", "00:22:aa", "00:22:d7", "00:23:31", "00:23:cc",
        "00:24:1e", "00:24:44", "00:24:f3", "00:25:a0", "00:26:59", "00:27:09", "34:af:2c",
        "40:d2:8a", "40:f4:07", "58:bd:a3", "8c:56:c5", "8c:cd:e8", "9c:e6:35", "a4:5c:27",
        "a4:c0:e1", "b8:ae:6e", "cc:9e:00", "cc:fb:65", "d8:6b:f7", "e0:0c:7f", "e0:e7:51",
        "e8:4e:ce",
    ]
    static func isNintendo(_ mac: String) -> Bool { prefixes.contains(String(mac.lowercased().prefix(8))) }
}
