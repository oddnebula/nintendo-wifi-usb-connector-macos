#!/bin/sh
# Builds "WFC Connector.app": the SwiftUI front end plus the wfcd helper.
# Requires: Xcode Command Line Tools, Homebrew libusb (statically linked; not needed at runtime).
set -e
cd "$(dirname "$0")"

make -C daemon wfcd
(cd app && swift build -c release)

APP="build/WFC Connector.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "app/.build/release/WFCConnector" "$APP/Contents/MacOS/WFCConnector"
cp daemon/wfcd "$APP/Contents/Resources/wfcd"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>WFC Connector</string>
  <key>CFBundleDisplayName</key><string>WFC Connector</string>
  <key>CFBundleIdentifier</key><string>local.wfcconnector</string>
  <key>CFBundleExecutable</key><string>WFCConnector</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSAppleEventsUsageDescription</key><string>Used to ask for administrator access to start the USB connector helper.</string>
</dict>
</plist>
PLIST

strip -x "$APP/Contents/MacOS/WFCConnector" "$APP/Contents/Resources/wfcd"
codesign --force --sign - "$APP/Contents/Resources/wfcd"
codesign --force --sign - "$APP"
echo "Built $APP"
