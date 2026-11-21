#!/bin/bash
# The self-contained boot image of iOS for the Pixel 4a: U-Boot, the HakiOS loader, and a payload with the kernel, the small root
# image and the 4 MB boot volume (flashboot.apfs, as the loader's resident image), so that nothing has to be sent from a PC. The root
# is the Darwin volume in the darwin partition, so build it first:  devices/sunfish/build-flash.sh ; make kernel ; make loader
#
#   devices/sunfish/mkinstall.sh [OUTPUT]       default build/ios-boot.img
#
# Test it without flashing anything: from the phone's bootloader,  fastboot boot build/ios-boot.img
# Flash it (once, at the end) with the KernelSU module's  ios-boot install  (devices/sunfish/ksu-module/), which backs the slot up first.
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
. devices/sunfish/boot-args.sh
OUT=${1:-build/ios-boot.img}
mkdir -p build
for f in hakios-kernel userland/root.apfs userland/flashboot.apfs xnuloader/loader.efi u-boot/build-sunfish_storage/u-boot.bin; do
	[ -f "$f" ] || { echo "mkinstall.sh: no $f"; exit 1; }
done
# INSTALL_ARGS: extra boot args of the flashed image (default cam=5: the camera; INSTALL_ARGS= for none)
# (debug and rd are what the loader and xl give a run; no epoch: the clock is set by the network, no loader_return_after: it runs until it is stopped)
python3 -I xnuloader/mkpayload.py -o build/ios-payload.bin kernel=hakios-kernel ramdisk=userland/root.apfs resident=userland/flashboot.apfs \
	--cmdline "debug=0x8 rd=md0 sb_delay=8 $BOOT_ARGS_BASE $FLASH_ARGS ${INSTALL_ARGS-cam=5}" >/dev/null || exit 1
python3 -I tools/mkubootimg.py -d sunfish -u u-boot/build-sunfish_storage/u-boot.bin -l xnuloader/loader.efi -p build/ios-payload.bin \
	--header-version 2 --pagesize 4096 -o "$OUT" || exit 1
avbtool add_hash_footer --image "$OUT" --partition_size 67108864 --partition_name boot --algorithm NONE >/dev/null 2>&1 || { echo "mkinstall.sh: avbtool failed"; exit 1; }
ls -l "$OUT" | awk '{print $5, $9}'
