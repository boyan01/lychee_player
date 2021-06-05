#!/bin/zsh

current_dir=$(pwd)/$(dirname "$0")

case "$1" in
"macos")
  build_dir="${current_dir}/build-darwin-macos"
  build_type="CMAKE_FLUTTER_MEDIA_MACOS"
  platform="MAC"
  ;;
"ios")
  build_dir="${current_dir}/build-darwin-ios"
  build_type="CMAKE_FLUTTER_MEDIA_IOS"
  case "$2" in
  "all")
    platform="OS64COMBINED"
    ;;
  "s") ;&
  "simulator")
    platform="SIMULATOR64"
    ;;
  "x64") ;&
  *)
    platform="OS64"
    ;;
  esac
  ;;
*)
  echo "build type must be one of: macos , ios."
  exit 1
  ;;
esac

# PLATFORM: OS64 MAC
# remove cmake install prefix. which default is 'usr/local'
cmake "$current_dir" -B "$build_dir" -G Xcode -DCMAKE_INSTALL_PREFIX="" -D${build_type}=1 \
  -DCMAKE_TOOLCHAIN_FILE="${current_dir}/ios.toolchain.cmake" -DPLATFORM=${platform}
# install build output to default dir.
cd "$build_dir" || exit
cmake --build "$build_dir" --config Release
cmake --install "$build_dir" --config Release --prefix "${current_dir}/../darwin/LycheePlayer"
