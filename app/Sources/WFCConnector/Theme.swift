import SwiftUI

/// Colours modelled on the light-blue Wi-Fi Connection look of the original connector software.
enum Theme {
    static let wfcBlue = Color(red: 0.0, green: 0.62, blue: 0.87)
    static let wfcDeep = Color(red: 0.0, green: 0.45, blue: 0.75)
    static let wfcSky = Color(red: 0.86, green: 0.95, blue: 1.0)
    static let panel = Color(nsColor: .controlBackgroundColor)
    static let ok = Color(red: 0.2, green: 0.68, blue: 0.3)
    static let waiting = Color(red: 0.95, green: 0.6, blue: 0.0)
    static let bad = Color(red: 0.85, green: 0.2, blue: 0.2)

    static var header: LinearGradient {
        LinearGradient(colors: [wfcBlue, wfcDeep], startPoint: .top, endPoint: .bottom)
    }
}

/// A small clamshell DS drawn from shapes.
struct DSGlyph: View {
    var color: Color = Theme.wfcBlue
    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            VStack(spacing: h * 0.04) {
                RoundedRectangle(cornerRadius: w * 0.12)
                    .fill(color)
                    .overlay(RoundedRectangle(cornerRadius: w * 0.05).fill(.white.opacity(0.9)).padding(w * 0.14))
                    .frame(height: h * 0.48)
                RoundedRectangle(cornerRadius: w * 0.12)
                    .fill(color.opacity(0.85))
                    .overlay(
                        HStack(spacing: w * 0.08) {
                            Image(systemName: "plus").font(.system(size: w * 0.18, weight: .black))
                            RoundedRectangle(cornerRadius: w * 0.04).fill(.white.opacity(0.9))
                                .frame(width: w * 0.36, height: h * 0.3)
                            Circle().frame(width: w * 0.14)
                        }.foregroundStyle(.white.opacity(0.9))
                    )
                    .frame(height: h * 0.48)
            }
        }
        .aspectRatio(0.8, contentMode: .fit)
    }
}

struct SignalBars: View {
    let rssi: Int
    var level: Int {
        switch rssi {
        case ..<(-85): return 0
        case ..<(-75): return 1
        case ..<(-65): return 2
        default: return 3
        }
    }
    var body: some View {
        HStack(alignment: .bottom, spacing: 2) {
            ForEach(0..<3) { i in
                RoundedRectangle(cornerRadius: 1)
                    .fill(i < level ? Theme.wfcBlue : Color.secondary.opacity(0.3))
                    .frame(width: 4, height: CGFloat(5 + i * 4))
            }
        }
        .help("\(rssi) dBm")
    }
}

struct StatusBadge: View {
    let text: String
    let color: Color
    var body: some View {
        Text(text)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(color.opacity(0.15), in: Capsule())
            .foregroundStyle(color)
    }
}

struct Card<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.panel, in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.wfcBlue.opacity(0.18)))
    }
}

/// View-local state holder. Used instead of @State because the Command Line Tools toolchain
/// lacks the SwiftUI macro plugin that @State requires with this SDK.
final class Box<T>: ObservableObject {
    @Published var value: T
    init(_ value: T) { self.value = value }
}
