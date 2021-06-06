#!/bin/zsh

# Helper for publish package to pub.dev

flutter pub get

cd example

cd macos && pod install && cd ..
cd ios && pod install && cd ..

cd ..

./ffplayer/apple-flutter-install.sh ios all
./ffplayer/apple-flutter-install.sh macos

mv macos/.gitignore macos/.gitignore.bak
mv ios/.gitignore ios/.gitignore.bak

flutter pub publish -n

mv macos/.gitignore.bak macos/.gitignore
mv ios/.gitignore.bak ios/.gitignore


