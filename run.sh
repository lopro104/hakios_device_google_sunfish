#!/bin/bash
# One test run of iOS 16.5 on the Pixel 4a from RAM, through U-Boot and the HakiOS loader. Nothing is written to the phone's storage.
#
#   devices/sunfish/run.sh ["extra boot-args" [SECONDS [NAME]]]        (run from ~/hakios; default 90 s)
#
# It RESETS the phone first (the loader writes the PMIC's reboot-reason register, so the reset lands in the stock bootloader in
# about 4 s; a loader older than 2026-11-06 goes through Android until slot b's retries run out, about 50 s). Warn whoever is
# using the phone. Before it: the kernel (make kernel), the loader (make loader) and userland/root.apfs + userland/system.apfs
# (GL=1 REST=1 userland/build.sh) are built, and u-boot for this device is built: CROSS_COMPILE=aarch64-linux-gnu- uefi/u-boot/build.sh sunfish_storage
# The log goes to logs/<time>-run.log (git-ignored; it holds whatever the radio saw, so mask before pasting anywhere).
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
. devices/sunfish/boot-args.sh
ARGS="$BOOT_ARGS_BASE $1"
SECS=${2:-90}      # how long the kernel runs before the loader gets the phone back; the boot itself is about a minute with FLASH=1
# FLASH=1: the root is the Darwin volume in the phone's darwin partition (userland/build.sh CRYPTEXES=flash FLASHBOOT=1): a 4 MB RAM image instead of 2.4 GB
RAMDISK=userland/system.apfs
[ -n "$FLASH" ] && { RAMDISK=userland/flashboot.apfs; ARGS="$ARGS $FLASH_ARGS"; }
timeout 2 fastboot getvar product 2>&1 | grep -q 'product: sunfish' || timeout 5 fastboot reboot >/dev/null 2>&1   # already in the bootloader: no reset
for i in $(seq 1 120); do sleep 2; timeout 2 fastboot getvar product 2>&1 | grep -q 'product: sunfish' && break; done
timeout 2 fastboot getvar product 2>&1 | grep -q 'product: sunfish' || { echo "run.sh: the phone did not reach its bootloader"; exit 1; }
python3 -I xnuloader/mkpayload.py -o build/usb-payload.bin --usb >/dev/null || exit 1
python3 -I tools/mkubootimg.py -d sunfish -u u-boot/build-sunfish_storage/u-boot.bin -l xnuloader/loader.efi -p build/usb-payload.bin \
	--header-version 2 --pagesize 4096 -o build/xnu-usb-storage.img >/dev/null || exit 1
avbtool add_hash_footer --image build/xnu-usb-storage.img --partition_size 67108864 --partition_name boot --algorithm NONE >/dev/null 2>&1
fastboot set_active b >/dev/null 2>&1        # Android's slot, refilled: each fastboot boot spends one of the active slot's three boot tries (nothing marks it successful), and with none left the phone stops in the bootloader. The bootloader's own command; nothing is flashed.
fastboot boot build/xnu-usb-storage.img >/dev/null 2>&1
for i in $(seq 1 15); do sleep 2; timeout 3 fastboot getvar product 2>&1 | grep -q 'product: xnuloader' && break; done
rm -f "${XDG_RUNTIME_DIR:-/tmp}/darwin-hakios-phone.ran"
export DEVICE=sunfish XL_FIRMWARE=u-boot XL_HOME=fastboot
./xl ramdisk $RAMDISK 2>&1 | tail -1
XL_SECS=$SECS XL_FOLLOW=10 XL_CMDLINE="$ARGS" ./xl run 2>&1 | tail -3
