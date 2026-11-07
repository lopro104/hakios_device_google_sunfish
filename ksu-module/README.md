# HakiOS iOS for the Pixel 4a: a KernelSU module

Boots the phone into iOS 16.5 (HakiOS) from its other boot slot, and back into Android.

- `ios-boot set-slot a|b`: choose the slot iOS lives in. Asked once, by hand; it refuses the slot Android is running from, and nothing else in the module guesses a slot (on this phone Android lives in slot b and slot a is the unbootable one, so `ios-boot set-slot a`).
- `ios-boot status`: which slot Android runs from, which holds iOS, whether the iOS image is installed there.
- `ios-boot install IMAGE`: writes the image (`devices/sunfish/mkinstall.sh` makes build/ios-boot.img) into the **chosen** slot's boot
  partition. Asks first, backs the slot up to /sdcard/Download/ios-boot-backups, reads back. Android's slot, the bootloaders, the
  modem and everything else are never written. Do it once, at the end: it is a flash write.
- `ios-boot` (or the module's Action button): iOS slot active, reboot.
- `android-boot`: Android's slot active, reboot. From iOS: hold the volume-down button while it starts (the bootloader), then
  `fastboot set_active b` (Android's slot), or let the iOS slot's boot counter run out.

The root of iOS is the `darwin` partition (docs: NOTES.md, "STORAGE"), not touched by this module.
