#!/bin/bash
# Builds "Re-ARMed Song Bridge.app" (arm64) into app/build/, with songbridge.dylib (../build.sh) inside it: the app
# installs the mod into RS Re-ARMed's mods folders.
#   ./build-app.sh           build
#   ./build-app.sh --open    build and open
#   ./build-app.sh --zip     build and make build/Re-ARMed-Song-Bridge-<version>.zip, the release download
#   ./build-app.sh --test    the installer's tests (no game needed)
set -euo pipefail
cd "$(dirname "$0")"
version=$(cat ../VERSION)

if [ "${1:-}" = --test ]; then
  exec swift test
fi

../build.sh
swift build -c release --arch arm64
BIN="$(swift build -c release --arch arm64 --show-bin-path)/SongBridgeApp"

APP="build/Re-ARMed Song Bridge.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Re-ARMed Song Bridge"
cp ../build/songbridge.dylib "$APP/Contents/Resources/"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Re-ARMed Song Bridge</string>
    <key>CFBundleDisplayName</key><string>Re-ARMed Song Bridge</string>
    <key>CFBundleIdentifier</key><string>io.github.innuendo486.rearmed-songbridge</string>
    <key>CFBundleExecutable</key><string>Re-ARMed Song Bridge</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP"
echo "Built $APP ($version)"

case "${1:-}" in
  --open) open "$APP" ;;
  --zip)
    zip="build/Re-ARMed-Song-Bridge-$version.zip"
    rm -f "$zip"
    ditto -c -k --keepParent "$APP" "$zip"
    echo "Release: $zip" ;;
esac
