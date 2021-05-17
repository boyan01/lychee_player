#!/bin/zsh

build_dir="build-macos"
# remove cmake install prefix. which default is 'usr/local'
cmake . -B $build_dir -DCMAKE_INSTALL_PREFIX="" -DCMAKE_FLUTTER_MEDIA_MACOS=1

# install build output to default dir.
cd $build_dir/flutter || exit
make install DESTDIR=../../../macos/Library/LycheePlayer/
