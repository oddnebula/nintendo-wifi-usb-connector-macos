import SwiftUI

struct SetupView: View {
    @EnvironmentObject var model: ConnectorModel

    var dns1Valid: Bool { model.validDNS(model.dns1, allowEmpty: model.dnsPreset == .none) }
    var dns2Valid: Bool { model.validDNS(model.dns2, allowEmpty: true) }

    var body: some View {
        Form {
            Section {
                TextField("Network name", text: $model.ssid)
                    .disabled(model.running)
                Picker("Channel", selection: $model.channel) {
                    ForEach(1...13, id: \.self) { Text("Channel \($0)").tag($0) }
                }
                .disabled(model.running)
                Text("The DS only joins open (or WEP) networks, so this network is open. Instead of a password, every new console has to be granted access here, just like the original connector.")
                    .font(.caption).foregroundStyle(.secondary)
            } header: {
                Text("Network")
            } footer: {
                if model.running { Text("Stop the connector to change these.").font(.caption) }
            }

            Section {
                Picker("Service", selection: Binding(get: { model.dnsPreset }, set: { model.dnsPreset = $0 })) {
                    ForEach(DNSPreset.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)

                if model.dnsPreset != .none {
                    LabeledContent("Primary DNS") {
                        HStack {
                            TextField("", text: $model.dns1, prompt: Text(DNSPreset.wiimmfiDNS))
                                .textFieldStyle(.roundedBorder)
                                .font(.body.monospaced())
                                .frame(maxWidth: 200)
                                .onChange(of: model.dns1) { if model.dns1 != DNSPreset.wiimmfiDNS && model.dnsPreset == .wiimmfi { model.dnsPresetRaw = DNSPreset.custom.rawValue } }
                            validity(dns1Valid)
                        }
                    }
                    LabeledContent("Secondary DNS") {
                        HStack {
                            TextField("", text: $model.dns2, prompt: Text("optional, e.g. 1.1.1.1"))
                                .textFieldStyle(.roundedBorder)
                                .font(.body.monospaced())
                                .frame(maxWidth: 200)
                                .onChange(of: model.dns2) { if !model.dns2.isEmpty && model.dnsPreset == .wiimmfi { model.dnsPresetRaw = DNSPreset.custom.rawValue } }
                            validity(dns2Valid)
                        }
                    }
                }
                HStack {
                    Text(explanation).font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("Apply") { model.applyDNS() }
                        .disabled(!model.running || !dns1Valid || !dns2Valid)
                }
            } header: {
                Text("Online service (DNS)")
            }

            Section("Access") {
                Toggle("Grant access to new consoles automatically", isOn: Binding(get: { model.autoAccept },
                                                                                   set: { model.setAutoAccept($0) }))
                Text("When off, a prompt appears whenever an unknown console connects.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("Setting up a Nintendo DS") {
                VStack(alignment: .leading, spacing: 8) {
                    step(1, "Press **Start** above so the connector begins broadcasting.")
                    step(2, "Insert a Wi-Fi game, open its **Nintendo WFC Settings → Nintendo Wi-Fi Connection Settings**, and choose an empty **Connection**.")
                    step(3, "Choose **Search for an Access Point** and select **\(model.ssid)**.")
                    step(4, "Click **Grant Access** when the console appears in this window.")
                    step(5, "Leave the console's DNS on **Auto-obtain**. The connector sends its DNS traffic to the server set above, so no DNS needs to be typed on the DS. (Typing the same DNS manually on the DS also works.)")
                    step(6, "Run the connection test. Some games also need a Wiimmfi patch; see wiimmfi.de.")
                }
                .font(.callout)
            }

            Section("Advanced") {
                Toggle("Demo mode (no hardware, simulated consoles)", isOn: $model.demoMode)
                    .onChange(of: model.demoMode) { model.relaunchHelper() }
                if !model.adapterMAC.isEmpty {
                    LabeledContent("Adapter", value: "\(model.adapterName)")
                    LabeledContent("MAC address", value: model.adapterMAC)
                    LabeledContent("Radio", value: model.adapterRF)
                }
            }
        }
        .formStyle(.grouped)
    }

    var explanation: String {
        switch model.dnsPreset {
        case .wiimmfi: return "Consoles' DNS lookups are sent to Wiimmfi, the community replacement for the Nintendo Wi-Fi Connection servers, which shut down in 2014."
        case .custom: return "All console DNS lookups go to the primary server. If you set a secondary server, it handles everything except the Nintendo WFC names."
        case .none: return "Consoles use the Mac's resolver. Nintendo WFC games will not get online this way."
        }
    }

    @ViewBuilder func validity(_ ok: Bool) -> some View {
        Image(systemName: ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            .foregroundStyle(ok ? Theme.ok : Theme.waiting)
            .help(ok ? "Valid IPv4 address" : "Enter an IPv4 address such as \(DNSPreset.wiimmfiDNS)")
    }

    func step(_ n: Int, _ text: LocalizedStringKey) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Text("\(n)").font(.caption.weight(.bold)).foregroundStyle(.white)
                .frame(width: 20, height: 20).background(Theme.wfcBlue, in: Circle())
            Text(text)
        }
    }
}
