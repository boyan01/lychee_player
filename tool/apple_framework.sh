# Sourced by tool/build_ffmpeg.sh, tool/package_ffmpeg_apple.sh and
# tool/build_apple.sh.

# Minimum OS versions, also in flutter/darwin/lychee_player.podspec. The
# Xcode 27 SDK's libc++ supports macOS 11 and iOS 15 and later;
# AppleFrameConverter needs VTPixelTransferSession, iOS 16.
apple_min_macos=11.0
apple_min_ios=16.0

#   make_framework SLICE FRAMEWORK BINARY HEADERS BUNDLE_ID VERSION
#
# Wraps one dynamic library into FRAMEWORK (path/NAME.framework) for an
# xcframework slice, copies the contents of the HEADERS directory into its
# Headers, and signs it ad hoc. SLICE is macos, ios or ios-simulator. macOS
# frameworks are versioned bundles (Versions/A), iOS ones are flat. BINARY
# must already carry its final install name, @rpath/NAME.framework/NAME.

make_framework() {
  local slice="$1" framework="$2" binary="$3" headers="$4" id="$5" version="$6"
  local name contents plist platform min_key min
  name="$(basename "$framework" .framework)"
  rm -rf "$framework"
  if [ "$slice" = macos ]; then
    contents="$framework/Versions/A"
    mkdir -p "$contents/Resources"
    ln -s A "$framework/Versions/Current"
    for f in "$name" Headers Resources; do
      ln -s "Versions/Current/$f" "$framework/$f"
    done
    plist="$contents/Resources/Info.plist"
    platform=MacOSX; min_key=LSMinimumSystemVersion; min=$apple_min_macos
  else
    contents="$framework"
    mkdir -p "$contents"
    plist="$contents/Info.plist"
    platform=iPhoneOS; min_key=MinimumOSVersion; min=$apple_min_ios
    [ "$slice" = ios-simulator ] && platform=iPhoneSimulator
  fi
  mkdir -p "$contents/Headers"
  cp "$binary" "$contents/$name"
  cp -R "$headers/." "$contents/Headers/"
  cat >"$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>$id</string>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$platform</string></array>
  <key>$min_key</key><string>$min</string>
</dict>
</plist>
EOF
  codesign --force --sign - "$framework" 2>/dev/null
}
