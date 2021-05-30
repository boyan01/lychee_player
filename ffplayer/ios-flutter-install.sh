#!/bin/zsh

build_dir="build-ios"
# remove cmake install prefix. which default is 'usr/local'
cmake . -B $build_dir -G Xcode -DCMAKE_INSTALL_PREFIX="" -DCMAKE_FLUTTER_MEDIA_IOS=1 -DCMAKE_TOOLCHAIN_FILE=./ios.toolchain.cmake -DPLATFORM=OS64

# install build output to default dir.
cd $build_dir || exit
cmake --build . --config Release
cmake --install . --config Release --prefix ../../ios/Library/LycheePlayer
