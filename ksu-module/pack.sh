#!/bin/bash
# Pack the module into build/hakios-ios-ksu.zip (module.prop at the zip's root, as KernelSU wants). The iOS image is not inside it:
# it is the phone's own build (devices/sunfish/mkinstall.sh) and is given to  ios-boot install  by path.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
mkdir -p ../../../build
rm -f ../../../build/hakios-ios-ksu.zip
zip -qr ../../../build/hakios-ios-ksu.zip module.prop action.sh README.md system && ls -l ../../../build/hakios-ios-ksu.zip | awk '{print $5, $9}'
