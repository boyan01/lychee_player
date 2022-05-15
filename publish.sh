#!/bin/zsh

# Helper for publish package to pub.dev

flutter pub get

cd example

cd macos && pod install && cd ..
#cd ios && pod install && cd ..

cd ..

#./lychee_cpp/apple-flutter-install.sh ios all
./lychee_cpp/apple-flutter-install.sh macos

flutter pub publish -n
