# HakiOS iOS for the Pixel 4a: a KernelSU module

Boots the phone into iOS 16.5 (HakiOS) from its other boot slot, and back into Android.

- `ios-boot status`: which slot Android runs from, which holds iOS, whether the iOS image is installed there.
- `ios-boot install IMAGE`: writes the image (`devices/sunfish/mkinstall.sh` makes build/ios-boot.img) into the **other** slot's boot
  partition. Asks first, backs the slot up to /sdcard/Download/ios-boot-backups, reads back. Android's slot, the bootloaders, the
  modem and everything else are never written. Do it once, at the end: it is a flash write.
- `ios-boot` (or the module's Action button): iOS slot active, reboot.
- `android-boot`: Android's slot active, reboot. From iOS: hold the volume-down button while it starts (the bootloader), then
  `fastboot set_active a` (slot letters as `ios-boot status` says), or let slot b's boot counter run out.

The root of iOS is the `darwin` partition (docs: NOTES.md, "STORAGE"), not touched by this module.
