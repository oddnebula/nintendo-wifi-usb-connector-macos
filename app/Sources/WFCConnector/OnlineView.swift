import SwiftUI

/// "Online Now": live player counts for every DS game on Wiimmfi, dressed up like a
/// handheld-era online lobby.
struct OnlineView: View {
    @EnvironmentObject var stats: OnlineStats
    @Environment(\.openURL) private var openURL

    private let columns = [GridItem(.adaptive(minimum: 190), spacing: 14)]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                PlazaBanner()

                if let top = stats.games.first, top.players > 0 {
                    SpotlightCard(game: top)
                }

                switch stats.phase {
                case .blocked:
                    NoticeCard(icon: "hand.raised.fill", title: "Wiimmfi wants to check you're a person",
                               text: "Open the stats page in your browser once, then come back and refresh.",
                               button: "Open in Browser") { openURL(OnlineStats.pageURL) }
                case .failed(let why) where stats.games.isEmpty:
                    NoticeCard(icon: "wifi.exclamationmark", title: "Couldn't reach Wiimmfi",
                               text: why, button: "Try Again") { stats.refresh() }
                default:
                    EmptyView()
                }

                if !stats.games.isEmpty {
                    HStack {
                        Text("Games with players").font(.headline)
                        Spacer()
                        Text("\(stats.games.count) \(stats.games.count == 1 ? "game" : "games")")
                            .font(.callout).foregroundStyle(.secondary)
                    }
                    LazyVGrid(columns: columns, spacing: 14) {
                        ForEach(Array(stats.games.enumerated()), id: \.element.id) { i, g in
                            GameTile(game: g, rank: i + 1, busiest: stats.games.first?.players ?? 1)
                                .onTapGesture { openURL(OnlineStats.gameURL(g.id)) }
                        }
                    }
                } else if stats.phase == .ready {
                    NoticeCard(icon: "moon.zzz.fill", title: "Everyone's asleep",
                               text: "No DS games have players right now. Turn on your DS and be the first one online!",
                               button: nil, action: {})
                } else if stats.phase == .loading || stats.phase == .idle {
                    LoadingTiles(columns: columns)
                }

                FooterLine()
            }
            .padding(20)
        }
        .background(DotGrid().ignoresSafeArea())
        .onAppear { stats.startAutoRefresh() }
        .onDisappear { stats.stopAutoRefresh() }
    }
}

// MARK: - Banner

private struct PlazaBanner: View {
    @EnvironmentObject var stats: OnlineStats
    @StateObject private var pulse = Box(false)

    var line: String {
        switch stats.total {
        case 0: return "It's quiet out there. Be the first to log on!"
        case 1..<10: return "A few friends are hanging out. Come say hi!"
        case 10..<40: return "The lobby is getting lively!"
        default: return "The lobby is packed. Grab a seat!"
        }
    }

    var body: some View {
        HStack(spacing: 18) {
            ZStack {
                Circle().fill(.white.opacity(0.25)).frame(width: 84, height: 84)
                    .scaleEffect(pulse.value ? 1.12 : 0.92)
                    .opacity(pulse.value ? 0.2 : 0.7)
                Circle().fill(.white).frame(width: 66, height: 66)
                Image(systemName: "globe.asia.australia.fill")
                    .font(.system(size: 38))
                    .foregroundStyle(Theme.wfcBlue)
            }
            .onAppear {
                withAnimation(.easeInOut(duration: 1.4).repeatForever(autoreverses: true)) { pulse.value = true }
            }

            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Circle().fill(Color(red: 0.5, green: 1, blue: 0.4)).frame(width: 8, height: 8)
                    Text("ONLINE NOW").font(.caption.weight(.heavy)).kerning(1.5)
                }
                .opacity(0.95)
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text("\(stats.total)")
                        .font(.system(size: 46, weight: .heavy, design: .rounded))
                        .contentTransition(.numericText())
                        .animation(.spring, value: stats.total)
                    Text(stats.total == 1 ? "DS player" : "DS players")
                        .font(.title3.weight(.bold))
                }
                SpeechBubble(text: stats.phase == .loading && stats.updated == nil ? "Looking around the lobby…" : line)
            }
            .foregroundStyle(.white)
            Spacer(minLength: 0)
        }
        .padding(18)
        .background(
            ZStack {
                Theme.header
                Stripes().opacity(0.12)
            }
            .clipShape(RoundedRectangle(cornerRadius: 18))
        )
        .shadow(color: Theme.wfcDeep.opacity(0.25), radius: 8, y: 4)
    }
}

private struct SpeechBubble: View {
    let text: String
    var body: some View {
        Text(text)
            .font(.callout.weight(.semibold))
            .foregroundStyle(Theme.wfcDeep)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(.white, in: RoundedRectangle(cornerRadius: 10))
            .overlay(alignment: .bottomLeading) {
                Triangle().fill(.white).frame(width: 12, height: 8).rotationEffect(.degrees(180)).offset(x: 14, y: 7)
            }
            .padding(.top, 4)
    }
}

// MARK: - Spotlight

private struct SpotlightCard: View {
    let game: OnlineGame
    var body: some View {
        HStack(spacing: 16) {
            Cartridge(game: game).frame(width: 64, height: 72)
            VStack(alignment: .leading, spacing: 4) {
                Label("Most played right now", systemImage: "crown.fill")
                    .font(.caption.weight(.heavy))
                    .foregroundStyle(Color(red: 0.95, green: 0.65, blue: 0.0))
                Text(game.name).font(.title3.weight(.bold)).lineLimit(2)
                Text(GameArt.cheer(for: game)).font(.callout).foregroundStyle(.secondary)
            }
            Spacer()
            PlayerCount(count: game.players, big: true)
        }
        .padding(16)
        .background(Theme.panel, in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(Color(red: 0.98, green: 0.78, blue: 0.2), lineWidth: 2))
    }
}

// MARK: - Game tiles

private struct GameTile: View {
    let game: OnlineGame
    let rank: Int
    let busiest: Int
    @StateObject private var hover = Box(false)

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top) {
                Cartridge(game: game).frame(width: 44, height: 50)
                Spacer()
                PlayerCount(count: game.players, big: false)
            }
            Text(game.name)
                .font(.headline)
                .lineLimit(2, reservesSpace: true)
            BusyMeter(fraction: Double(game.players) / Double(max(busiest, 1)), color: GameArt.color(for: game))
            // Kept (invisibly) when empty so every tile in a row is the same height.
            Label("Some features don't work", systemImage: "info.circle")
                .font(.caption).foregroundStyle(.secondary)
                .help(game.remark)
                .opacity(game.remark.isEmpty ? 0 : 1)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.panel, in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(GameArt.color(for: game).opacity(hover.value ? 0.9 : 0.25), lineWidth: hover.value ? 2 : 1))
        .scaleEffect(hover.value ? 1.03 : 1)
        .animation(.spring(response: 0.25), value: hover.value)
        .onHover { hover.value = $0 }
        .help("Open \(game.name) on wiimmfi.de")
    }
}

private struct PlayerCount: View {
    let count: Int
    let big: Bool
    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: count == 1 ? "person.fill" : "person.2.fill")
            Text("\(count)").contentTransition(.numericText())
        }
        .font(big ? .title2.weight(.heavy) : .callout.weight(.bold))
        .foregroundStyle(Theme.wfcDeep)
        .padding(.horizontal, big ? 14 : 9)
        .padding(.vertical, big ? 8 : 4)
        .background(Theme.wfcSky, in: Capsule())
    }
}

private struct BusyMeter: View {
    let fraction: Double
    let color: Color
    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<8) { i in
                Capsule()
                    .fill(Double(i) < max(1, fraction * 8).rounded() ? color : Color.secondary.opacity(0.18))
                    .frame(height: 6)
            }
        }
    }
}

/// A little game card drawn from shapes: notched top, coloured label, game icon.
private struct Cartridge: View {
    let game: OnlineGame
    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            ZStack {
                CartShape().fill(Color(white: 0.32))
                RoundedRectangle(cornerRadius: w * 0.08)
                    .fill(LinearGradient(colors: [GameArt.color(for: game), GameArt.color(for: game).opacity(0.75)],
                                         startPoint: .top, endPoint: .bottom))
                    .frame(width: w * 0.78, height: h * 0.62)
                    .offset(y: h * 0.08)
                Image(systemName: GameArt.icon(for: game))
                    .font(.system(size: w * 0.36, weight: .bold))
                    .foregroundStyle(.white)
                    .offset(y: h * 0.08)
            }
        }
    }
}

private struct CartShape: Shape {
    func path(in r: CGRect) -> Path {
        let n = r.width * 0.18
        var p = Path()
        p.move(to: CGPoint(x: r.minX + 4, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX - n, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.minY + n))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - 4))
        p.addQuadCurve(to: CGPoint(x: r.maxX - 4, y: r.maxY), control: CGPoint(x: r.maxX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX + 4, y: r.maxY))
        p.addQuadCurve(to: CGPoint(x: r.minX, y: r.maxY - 4), control: CGPoint(x: r.minX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.minY + 4))
        p.addQuadCurve(to: CGPoint(x: r.minX + 4, y: r.minY), control: CGPoint(x: r.minX, y: r.minY))
        return p
    }
}

/// Picks an icon, colour and a cheerful line for a game from its title.
enum GameArt {
    private static let palette: [Color] = [
        Color(red: 0.0, green: 0.6, blue: 0.9),   // ice blue
        Color(red: 0.93, green: 0.3, blue: 0.55), // coral pink
        Color(red: 0.86, green: 0.2, blue: 0.2),  // crimson
        Color(red: 0.3, green: 0.7, blue: 0.3),   // green
        Color(red: 0.97, green: 0.58, blue: 0.1), // orange
        Color(red: 0.55, green: 0.35, blue: 0.85),// purple
        Color(red: 0.1, green: 0.65, blue: 0.65), // teal
    ]

    private static let rules: [(String, String, String)] = [
        ("Mario Kart", "flag.checkered", "Engines are revving on the starting line!"),
        ("Pokémon", "sparkles", "Trainers are trading and battling!"),
        ("Animal Crossing", "leaf.fill", "Neighbours are visiting each other's towns!"),
        ("Tetris", "square.grid.3x2.fill", "Blocks are falling fast!"),
        ("Metroid", "scope", "Bounty hunters are on the prowl!"),
        ("Fossil", "fossil.shell.fill", "Vivosaurs are battling it out!"),
        ("Mystery Dungeon", "map.fill", "Rescue teams are heading out!"),
        ("Bomberman", "flame.fill", "Things are getting explosive!"),
        ("Star Fox", "airplane", "Pilots are taking to the skies!"),
        ("Mario", "star.fill", "Let's-a go!"),
        ("Zelda", "shield.fill", "Heroes are on an adventure!"),
        ("Golf", "figure.golf", "Fore! Players are on the green."),
        ("Tennis", "tennisball.fill", "Rallies are underway!"),
        ("Chess", "checkerboard.rectangle", "Grandmasters are thinking hard."),
        ("Puzzle", "puzzlepiece.fill", "Brains are buzzing!"),
        ("Racing", "car.fill", "Drivers are on the track!"),
        ("Dragon Quest", "wand.and.stars", "Adventurers are teaming up!"),
        ("Final Fantasy", "wand.and.stars", "Parties are setting out!"),
    ]

    private static func rule(for g: OnlineGame) -> (String, String, String)? {
        rules.first { g.name.localizedCaseInsensitiveContains($0.0) }
    }

    static func icon(for g: OnlineGame) -> String { rule(for: g)?.1 ?? "gamecontroller.fill" }
    static func cheer(for g: OnlineGame) -> String { rule(for: g)?.2 ?? "Players are having fun together!" }

    static func color(for g: OnlineGame) -> Color {
        // A stable per-game colour (String.hashValue changes every launch).
        let sum = g.id.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) & 0xffff }
        return palette[sum % palette.count]
    }
}

// MARK: - Loading, notices, footer

private struct LoadingTiles: View {
    let columns: [GridItem]
    @StateObject private var shimmer = Box(false)
    var body: some View {
        LazyVGrid(columns: columns, spacing: 14) {
            ForEach(0..<4, id: \.self) { _ in
                RoundedRectangle(cornerRadius: 14)
                    .fill(Theme.wfcSky.opacity(shimmer.value ? 0.9 : 0.4))
                    .frame(height: 130)
            }
        }
        .onAppear {
            withAnimation(.easeInOut(duration: 0.8).repeatForever(autoreverses: true)) { shimmer.value = true }
        }
    }
}

private struct NoticeCard: View {
    let icon: String
    let title: String
    let text: String
    let button: String?
    let action: () -> Void
    var body: some View {
        Card {
            HStack(spacing: 14) {
                Image(systemName: icon).font(.system(size: 26)).foregroundStyle(Theme.wfcBlue).frame(width: 36)
                VStack(alignment: .leading, spacing: 4) {
                    Text(title).font(.headline)
                    Text(text).font(.callout).foregroundStyle(.secondary)
                }
                Spacer()
                if let button {
                    Button(button, action: action).buttonStyle(.borderedProminent).tint(Theme.wfcBlue)
                }
            }
        }
    }
}

private struct FooterLine: View {
    @EnvironmentObject var stats: OnlineStats
    @Environment(\.openURL) private var openURL

    var body: some View {
        HStack(spacing: 10) {
            if stats.phase == .loading {
                ProgressView().controlSize(.small)
                Text("Checking…")
            } else if let t = stats.updated {
                Image(systemName: "clock")
                Text("Updated \(t, style: .relative) ago")
            }
            Spacer()
            Button("wiimmfi.de") { openURL(OnlineStats.pageURL) }.buttonStyle(.link)
            Button { stats.refresh() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                .disabled(stats.phase == .loading)
        }
        .font(.callout)
        .foregroundStyle(.secondary)
    }
}

// MARK: - Decoration

/// The faint dotted grid from the handheld's menu screens.
private struct DotGrid: View {
    var body: some View {
        Canvas { ctx, size in
            let step: CGFloat = 16
            for x in stride(from: step / 2, to: size.width, by: step) {
                for y in stride(from: step / 2, to: size.height, by: step) {
                    ctx.fill(Path(ellipseIn: CGRect(x: x - 1, y: y - 1, width: 2, height: 2)),
                             with: .color(Theme.wfcBlue.opacity(0.12)))
                }
            }
        }
    }
}

private struct Stripes: View {
    var body: some View {
        Canvas { ctx, size in
            var x: CGFloat = -size.height
            while x < size.width {
                var p = Path()
                p.move(to: CGPoint(x: x, y: size.height))
                p.addLine(to: CGPoint(x: x + size.height, y: 0))
                ctx.stroke(p, with: .color(.white), lineWidth: 10)
                x += 26
            }
        }
    }
}

private struct Triangle: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        p.move(to: CGPoint(x: r.midX, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        p.closeSubpath()
        return p
    }
}
