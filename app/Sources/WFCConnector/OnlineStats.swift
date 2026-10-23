import Foundation
import WebKit

struct OnlineGame: Identifiable, Equatable {
    let id: String      // GameSpy game name, e.g. "mariokartds"
    let name: String
    let remark: String
    let players: Int
}

/// Reads the "games with active users" table from Wiimmfi's public stats page.
///
/// Wiimmfi has no JSON API and its site sits behind Cloudflare's browser check, so the page is
/// loaded in an off-screen WebKit view the same way Safari would load it. The check clears on its
/// own after a few seconds; if it ever wants a click, the user gets a button to open the page
/// in their browser instead.
@MainActor
final class OnlineStats: NSObject, ObservableObject, WKNavigationDelegate {
    enum Phase: Equatable { case idle, loading, ready, blocked, failed(String) }

    static let pageURL = URL(string: "https://wiimmfi.de/stats/game")!
    static func gameURL(_ id: String) -> URL { URL(string: "https://wiimmfi.de/stats/game/\(id)")! }

    @Published private(set) var games: [OnlineGame] = []
    @Published private(set) var phase: Phase = .idle
    @Published private(set) var updated: Date?

    var total: Int { games.reduce(0) { $0 + $1.players } }

    private var web: WKWebView?
    private var attempts = 0
    private var refreshTimer: Timer?
    private var pollTimer: Timer?

    /// Starts loading now and then every minute until `stopAutoRefresh` is called.
    func startAutoRefresh() {
        refresh()
        refreshTimer?.invalidate()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        }
    }

    func stopAutoRefresh() {
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    func refresh() {
        if phase == .loading { return }
        phase = .loading
        attempts = 0
        if web == nil {
            let w = WKWebView(frame: NSRect(x: 0, y: 0, width: 1024, height: 768))
            w.navigationDelegate = self
            web = w
        }
        web?.load(URLRequest(url: Self.pageURL, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 20))
        schedulePoll()
    }

    // The challenge page swaps itself for the real page without a normal navigation finishing,
    // so the table is polled for rather than read once on didFinish.
    private func schedulePoll() {
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
    }

    private static let extractJS = """
    (() => {
      if (document.title.startsWith('Just a moment')) return 'challenge';
      const t = document.querySelector('table#game');
      if (!t) return document.title.includes('Statistics') ? '[]' : 'loading';
      const rows = [...t.querySelectorAll('tr')].map(r => r.querySelectorAll('td'))
        .filter(c => c.length >= 5 && c[1].querySelector('a'));
      return JSON.stringify(rows.map(c => ({
        type: c[0].textContent.trim(),
        id: (c[1].querySelector('a').getAttribute('href') || '').split('/').pop(),
        name: c[1].textContent.trim(),
        remark: c[2].textContent.trim(),
        online: parseInt(c[4].textContent, 10) || 0
      })));
    })()
    """

    private func poll() {
        guard phase == .loading, let web else { return }
        attempts += 1
        web.evaluateJavaScript(Self.extractJS) { [weak self] result, _ in
            MainActor.assumeIsolated {
                guard let self, self.phase == .loading else { return }
                let s = result as? String ?? "loading"
                if s.hasPrefix("[") {
                    self.apply(json: s)
                } else if self.attempts >= 16 {
                    self.phase = s == "challenge" ? .blocked : .failed("Wiimmfi didn't answer.")
                } else {
                    self.schedulePoll()
                }
            }
        }
    }

    private func apply(json: String) {
        struct Row: Decodable { let type, id, name, remark: String; let online: Int }
        guard let rows = try? JSONDecoder().decode([Row].self, from: Data(json.utf8)) else {
            phase = .failed("The stats page changed and couldn't be read.")
            return
        }
        games = rows.filter { $0.type == "NDS" }
            .map { OnlineGame(id: $0.id, name: $0.name, remark: $0.remark, players: $0.online) }
            .sorted { $0.players != $1.players ? $0.players > $1.players : $0.name < $1.name }
        updated = Date()
        phase = .ready
    }

    nonisolated func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        MainActor.assumeIsolated { fail(error) }
    }

    nonisolated func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        MainActor.assumeIsolated { fail(error) }
    }

    private func fail(_ error: Error) {
        // A challenge redirect cancels the first load; that's expected and not a failure.
        if (error as NSError).code == NSURLErrorCancelled { return }
        pollTimer?.invalidate()
        phase = .failed(error.localizedDescription)
    }
}
