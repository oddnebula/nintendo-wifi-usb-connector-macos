import Foundation
import Darwin

/// Talks line-delimited JSON to the root `wfcd` helper over a Unix socket, and launches the
/// helper (with an administrator prompt) when it is not already running.
final class DaemonClient: @unchecked Sendable {
    var onEvent: (([String: Any]) -> Void)?
    var onDisconnect: (() -> Void)?

    private var fd: Int32 = -1
    private let writeLock = NSLock()
    private let readQueue = DispatchQueue(label: "wfc.daemon.read")

    static var socketPath: String { "/tmp/nintendowfc-\(getuid()).sock" }
    static var logPath: String { "/tmp/nintendowfc-\(getuid()).log" }

    var isConnected: Bool { fd >= 0 }

    /// Path of the bundled helper (Contents/Resources/wfcd), or next to the executable in dev builds.
    static func helperPath() -> String? {
        if let p = Bundle.main.path(forResource: "wfcd", ofType: nil) { return p }
        let exe = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
        for candidate in [exe.appendingPathComponent("wfcd"),
                          exe.appendingPathComponent("../../../../daemon/wfcd").standardized] {
            if FileManager.default.isExecutableFile(atPath: candidate.path) { return candidate.path }
        }
        return nil
    }

    func connect() -> Bool {
        let s = socket(AF_UNIX, SOCK_STREAM, 0)
        guard s >= 0 else { return false }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let path = Self.socketPath
        withUnsafeMutableBytes(of: &addr.sun_path) { raw in
            let bytes = Array(path.utf8.prefix(raw.count - 1))
            raw.copyBytes(from: bytes)
            raw[bytes.count] = 0
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(s, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) == 0
            }
        }
        if !ok { close(s); return false }
        var on: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        fd = s
        readQueue.async { [weak self] in self?.readLoop(s) }
        return true
    }

    /// Starts the helper. Real hardware needs root (USB + vmnet), so macOS shows its
    /// administrator password prompt; demo mode runs unprivileged.
    func launchHelper(simulate: Bool) throws {
        guard let helper = Self.helperPath() else {
            throw NSError(domain: "WFC", code: 1, userInfo: [NSLocalizedDescriptionKey:
                "The wfcd helper is missing from the app bundle."])
        }
        var args = ["--gui-socket", Self.socketPath, "--owner", String(getuid())]
        if simulate { args.append("--simulate") }
        if simulate {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: helper)
            p.arguments = args
            FileManager.default.createFile(atPath: Self.logPath, contents: nil)
            p.standardError = FileHandle(forWritingAtPath: Self.logPath)
            p.standardOutput = p.standardError
            try p.run()
            return
        }
        func q(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        let cmd = ([helper] + args).map(q).joined(separator: " ") + " >" + q(Self.logPath) + " 2>&1 &"
        let script = "do shell script \"\(cmd.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges with prompt \"WFC Connector needs administrator access to drive the Nintendo Wi-Fi USB Connector and share this Mac's internet connection.\""
        var error: NSDictionary?
        guard let apple = NSAppleScript(source: script) else { return }
        apple.executeAndReturnError(&error)
        if let error {
            let msg = error[NSAppleScript.errorMessage] as? String ?? "Authorization failed."
            throw NSError(domain: "WFC", code: 2, userInfo: [NSLocalizedDescriptionKey: msg])
        }
    }

    func send(_ obj: [String: Any]) {
        guard fd >= 0, var data = try? JSONSerialization.data(withJSONObject: obj) else { return }
        data.append(0x0a)
        writeLock.lock()
        defer { writeLock.unlock() }
        data.withUnsafeBytes { raw in
            var off = 0
            while off < raw.count {
                let n = write(fd, raw.baseAddress!.advanced(by: off), raw.count - off)
                if n <= 0 { break }
                off += n
            }
        }
    }

    func disconnect() {
        if fd >= 0 { shutdown(fd, SHUT_RDWR) }
    }

    private func readLoop(_ s: Int32) {
        var pending = Data()
        var buf = [UInt8](repeating: 0, count: 8192)
        while true {
            let n = read(s, &buf, buf.count)
            if n <= 0 { break }
            pending.append(buf, count: n)
            while let nl = pending.firstIndex(of: 0x0a) {
                let line = pending[pending.startIndex..<nl]
                pending.removeSubrange(pending.startIndex...nl)
                if let obj = try? JSONSerialization.jsonObject(with: line) as? [String: Any] {
                    onEvent?(obj)
                }
            }
        }
        close(s)
        writeLock.lock()
        if fd == s { fd = -1 }
        writeLock.unlock()
        onDisconnect?()
    }
}
