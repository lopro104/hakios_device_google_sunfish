#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

# Ensure host tools (xnu-clang, ldid) can be located
export PATH="$HOME/hakios/xnu-deps/host/bin:$HOME/theos/toolchain/linux/iphone/bin:$PATH"

mkdir -p build/BootSwitch.app

xnu-clang -arch arm64 -isysroot "$HOME/theos/sdks/iPhoneOS16.5.sdk" -O1 \
    -framework UIKit -framework Foundation \
    -fobjc-arc \
    main.m AppDelegate.m \
    -o build/BootSwitch.app/BootSwitch

cp Info.plist build/BootSwitch.app/Info.plist

if command -v ldid >/dev/null 2>&1; then
    ldid -S build/BootSwitch.app/BootSwitch 2>/dev/null || true
fi

echo "BootSwitch.app built successfully at: $DIR/build/BootSwitch.app"
