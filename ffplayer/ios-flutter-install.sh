#!/bin/zsh

current_dir=$(pwd)/$(dirname "$0")

build_dir="${current_dir}/build-ios"

# remove cmake install prefix. which default is 'usr/local'
cmake "$current_dir" -B "$build_dir" -G Xcode -DCMAKE_INSTALL_PREFIX="" -DCMAKE_FLUTTER_MEDIA_IOS=1 \
  -DCMAKE_TOOLCHAIN_FILE="${current_dir}/ios.toolchain.cmake" -DPLATFORM=OS64COMBINED

# install build output to default dir.
cd "$build_dir" || exit
cmake --build "$build_dir" --config Release
cmake --install "$build_dir" --config Release --prefix "${current_dir}/../ios/Library/LycheePlayer"
