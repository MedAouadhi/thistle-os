#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Thistle Chat for macOS as a disk image: build/macos/ThistleChat.dmg, the
# app beside a link to /Applications, to drag across.
#
# Run on a Mac, from companion/app, with Xcode, CocoaPods, CMake and
# Flutter. The app carries its own libopus (Contents/Frameworks), built
# for arm64 and x86_64 from the repository's libopus by the same CMake
# file as Android's, so voice messages and T-Deck calls need no Homebrew.
#
# The image is not notarized and the app is signed ad hoc, so macOS refuses
# it the first time: System Settings, Privacy & Security, Open Anyway (or
# `xattr -dr com.apple.quarantine "/Applications/Thistle Chat.app"`).
set -eu

cd "$(dirname "$0")/.."
[ "$(uname)" = Darwin ] || { echo "make_dmg.sh: run this on a Mac" >&2; exit 1; }

flutter pub get --enforce-lockfile
flutter build macos --release

app=build/macos/Build/Products/Release/thistle_chat.app

# libopus, universal, as the core opens it (core/lib/src/opus.dart).
cmake -S android/app/src/main/cpp -B build/macos-opus \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build build/macos-opus --config Release
mkdir -p "$app/Contents/Frameworks"
cp build/macos-opus/libopus.dylib "$app/Contents/Frameworks/libopus.dylib"
install_name_tool -id @rpath/libopus.dylib "$app/Contents/Frameworks/libopus.dylib"

# Adding a file broke the build's signature: sign it again, ad hoc, keeping
# the entitlements (the microphone and the camera).
codesign --force --sign - "$app/Contents/Frameworks/libopus.dylib"
codesign --force --sign - --entitlements macos/Runner/Release.entitlements "$app"
codesign --verify --strict "$app"

stage=build/macos/dmg
rm -rf "$stage" build/macos/ThistleChat.dmg
mkdir -p "$stage"
cp -R "$app" "$stage/Thistle Chat.app"
ln -s /Applications "$stage/Applications"
hdiutil create -volname 'Thistle Chat' -srcfolder "$stage" -fs HFS+ -format UDZO -ov build/macos/ThistleChat.dmg
rm -rf "$stage"
echo "build/macos/ThistleChat.dmg"
