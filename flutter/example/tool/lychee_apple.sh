#!/bin/bash
# Run after Flutter's embed phase. Locate the SDK through Flutter's own
# app-local plugin metadata instead of assuming a pub-cache directory name.
set -euo pipefail
[ "${ACTION:-}" != indexbuild ] || exit 0
case "${PLATFORM_NAME:?}" in macosx) platform=macos ;; *) platform=ios ;; esac
metadata="${FLUTTER_APPLICATION_PATH:?}/.flutter-plugins-dependencies"
enabled="$(/usr/bin/plutil -extract "swift_package_manager_enabled.$platform" raw -o - "$metadata" 2>/dev/null || true)"
if [ "$enabled" != true ]; then
  # Keep the declared build-phase output valid with CocoaPods as well.
  printf 'CocoaPods\n' >"${TARGET_BUILD_DIR:?}/${UNLOCALIZED_RESOURCES_FOLDER_PATH:?}/lychee_player_backend"
  exit 0
fi
index=0
while name="$(/usr/bin/plutil -extract "plugins.$platform.$index.name" raw -o - "$metadata" 2>/dev/null)"; do
  if [ "$name" = lychee_player ]; then
    plugin="$(/usr/bin/plutil -extract "plugins.$platform.$index.path" raw -o - "$metadata")"
    exec /bin/bash "$plugin/darwin/embed_frameworks.sh"
  fi
  index=$((index + 1))
done
echo 'error: lychee_player: plugin metadata missing; run flutter pub get' >&2
exit 1
