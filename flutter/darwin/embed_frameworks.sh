#!/bin/bash
# Called by the app's final Flutter embed build phase, before app signing.
# SwiftPM compiles against the common C ABI; this stage embeds the app's
# selected implementation with the same framework name and matching version.
set -euo pipefail

die() { echo "error: lychee_player: $*" >&2; exit 1; }
value() { /usr/bin/plutil -extract "$1" raw -o - "$2"; }

backend="${LYCHEE_WITH_FFMPEG-${LYCHEE_PLAYER_FFMPEG:-OFF}}"
backend="$(/usr/bin/tr '[:lower:]' '[:upper:]' <<<"$backend")"
case "$backend" in ON|OFF) ;; *) die "FFmpeg setting must be ON or OFF" ;; esac

case "${PLATFORM_NAME:?}" in
  macosx) platform=macos; variant= ;;
  iphoneos) platform=ios; variant= ;;
  iphonesimulator) platform=ios; variant=simulator ;;
  *) die "unsupported Apple platform: $PLATFORM_NAME" ;;
esac
case "${FRAMEWORKS_FOLDER_PATH:?}" in
  "${WRAPPER_NAME:?}"/*) ;;
  *) die "framework output must be inside this app bundle" ;;
esac
root="$(cd "$(dirname "$0")/.." && pwd)"
manifest="$root/prebuilt.json"
state="${DERIVED_FILE_DIR:?}/lychee_player/backend"
if [ -f "$state" ] && [ "$(cat "$state")" != "$backend" ]; then
  die "FFmpeg setting changed; run flutter clean before rebuilding"
fi
mkdir -p "$(dirname "$state")"
# Record the choice before writing outputs, including an interrupted build.
printf '%s' "$backend" >"$state"
artifacts=(lychee_noffmpeg)
[ "$backend" != ON ] || artifacts=(lychee libavformat libavcodec libswresample libswscale libavutil)

frameworks="$root/darwin/Frameworks"
local=1
[ ! -f "$frameworks/.prebuilt" ] || local=0
for name in "${artifacts[@]}"; do
  [ -d "$frameworks/$name.xcframework" ] || local=0
done
if [ "$local" = 0 ]; then
  tag="$(value tag "$manifest")"
  base_url="${LYCHEE_PREBUILT_URL:-$(value url "$manifest")}"
  frameworks="${DERIVED_FILE_DIR:?}/lychee_player/$tag"
  mkdir -p "$frameworks"
  for name in "${artifacts[@]}"; do
    # plutil key paths escape dots inside JSON dictionary keys.
    sha="$(value "files.$name\\.xcframework\\.zip" "$manifest")"
    archive="$frameworks/$sha.zip"
    if [ ! -f "$archive" ] || [ "$(/usr/bin/shasum -a 256 "$archive" | /usr/bin/cut -d' ' -f1)" != "$sha" ]; then
      /usr/bin/curl -fsSL --retry 3 --connect-timeout 15 \
        -o "$archive.part" "$base_url/$tag/$name.xcframework.zip"
      [ "$(/usr/bin/shasum -a 256 "$archive.part" | /usr/bin/cut -d' ' -f1)" = "$sha" ] || die "checksum mismatch for $name"
      /bin/mv "$archive.part" "$archive"
    fi
    # Archives are immutable and app/target-local, never the shared pub cache.
    if [ ! -f "$frameworks/$sha.complete" ]; then
      /usr/bin/unzip -oq "$archive" -d "$frameworks"
      /usr/bin/touch "$frameworks/$sha.complete"
    fi
  done
fi

output="${TARGET_BUILD_DIR:?}/$FRAMEWORKS_FOLDER_PATH"
mkdir -p "$output"
for name in "${artifacts[@]}"; do
  xcframework="$frameworks/$name.xcframework"
  info="$xcframework/Info.plist"
  index=0
  source=
  while id="$(value "AvailableLibraries.$index.LibraryIdentifier" "$info" 2>/dev/null)"; do
    slice_platform="$(value "AvailableLibraries.$index.SupportedPlatform" "$info")"
    slice_variant="$(value "AvailableLibraries.$index.SupportedPlatformVariant" "$info" 2>/dev/null || true)"
    if [ "$slice_platform" = "$platform" ] && [ "$slice_variant" = "$variant" ]; then
      architectures="$(/usr/bin/plutil -extract "AvailableLibraries.$index.SupportedArchitectures" json -o - "$info")"
      supported=1
      for arch in ${ARCHS:?}; do
        /usr/bin/grep -Fq "\"$arch\"" <<<"$architectures" || supported=0
      done
      if [ "$supported" = 1 ]; then
        library="$(value "AvailableLibraries.$index.LibraryPath" "$info")"
        source="$xcframework/$id/$library"
        break
      fi
    fi
    index=$((index + 1))
  done
  [ -n "$source" ] || die "no $PLATFORM_NAME slice for $name ($ARCHS)"
  expected="$name.framework"
  [ "$name" != lychee_noffmpeg ] || expected=lychee.framework
  [ "$library" = "$expected" ] || die "unexpected framework name: $library"
  if [ "$library" = lychee.framework ]; then
    api_version() { /usr/bin/awk '/^#define LYCHEE_API_VERSION / {print $3}' "$1"; }
    compiled_api="$(api_version "${BUILT_PRODUCTS_DIR:?}/lychee.framework/Headers/lychee.h")"
    [ -n "$compiled_api" ] && [ "$(api_version "$source/Headers/lychee.h")" = "$compiled_api" ] ||
      die "selected core does not match the SwiftPM API version; build matching local frameworks"
  fi
  destination="$output/$library"
  # Only replace this SDK's known frameworks inside the validated app path.
  /bin/rm -rf "$destination"
  /usr/bin/ditto "$source" "$destination"
  binary="$destination/${library%.framework}"
  [ ! -d "$destination/Versions/A" ] || binary="$destination/Versions/A/${library%.framework}"
  for arch in $(/usr/bin/xcrun lipo -archs "$binary"); do
    case " $ARCHS " in
      *" $arch "*) ;;
      *) /usr/bin/xcrun lipo "$binary" -remove "$arch" -output "$binary" ;;
    esac
  done
  identity="${EXPANDED_CODE_SIGN_IDENTITY:--}"
  [ "${CODE_SIGNING_ALLOWED:-YES}" != NO ] || identity=-
  /usr/bin/codesign --force --sign "$identity" --preserve-metadata=identifier,entitlements "$destination"
  # Xcode's debugger searches BUILT_PRODUCTS_DIR before the app bundle.
  # Keep that app-owned copy on the same backend, without touching the shared
  # SwiftPM artifact cache. Preserve the core's headers to keep its PCM valid.
  products="$(cd "${BUILT_PRODUCTS_DIR:?}" && pwd -P)"
  product="$products/$library"
  product_binary="$product/${library%.framework}"
  [ ! -d "$product/Versions/A" ] || product_binary="$product/Versions/A/${library%.framework}"
  parent="$(cd "$(dirname "$product_binary")" 2>/dev/null && pwd -P || true)"
  case "$parent" in
    "$product"|"$product"/*)
      /bin/rm -f "$product_binary"
      /usr/bin/ditto "$binary" "$product_binary"
      /usr/bin/codesign --force --sign "$identity" --preserve-metadata=identifier,entitlements "$product"
      ;;
    *)
      /bin/rm -rf "$product"
      /usr/bin/ditto "$destination" "$product"
      ;;
  esac
done
# Declare this app resource as a build-phase output so Xcode invalidates the
# outer app signature when this phase replaces an embedded binary.
printf '%s\n' "$backend" >"${TARGET_BUILD_DIR:?}/${UNLOCALIZED_RESOURCES_FOLDER_PATH:?}/lychee_player_backend"
# A backend change requires a clean build. Do not delete FFmpeg frameworks
# here: another SDK in the app may own them when Lychee uses the base core.
echo "lychee_player: embedded FFmpeg=$backend for $PLATFORM_NAME ($ARCHS)"
