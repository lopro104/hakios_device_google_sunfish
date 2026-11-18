#!/bin/bash
# The RAM part of a boot from the darwin partition on the Pixel 4a: flashboot.apfs (4 MB) and root.apfs (with Mesa), made with
# the everyday job list. Run from ~/hakios:   devices/sunfish/build-flash.sh      (run it with devices/sunfish/run.sh, FLASH=1)
# DENY_MORE takes out jobs that only cost heat here: Visual Voicemail (no baseband) and FairPlay (restarts every 10 s).
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
# the cross tools, and ldid (the entitlements of the test tools and of bootd)
export PATH="$HOME/hakios/xnu-deps/host/bin:$HOME/theos/toolchain/linux/iphone/bin:$PATH"
# METAL=1: CoreAnimation's window server on Metal (hakios-metal for the Adreno 618, make ios CHIP=618 MESA=1 in hakios-metal/) instead of OpenGL ES;
# run it with  FLASH=1 METAL=1 devices/sunfish/run.sh  (boot arguments metal=1 and ca_metallib=)
[ -n "$METAL" ] && export METAL_PLUGIN=ca EXTRA_SHARE="$HOME/ca-share" CHIP=618 HAKIOS_METAL="$PWD/hakios-metal"
# our own applications for the home screen (devices/sunfish/apps/): built here, shown by init
mkdir -p build/apps && for app in devices/sunfish/apps/*/; do
	[ -x "$app/build.sh" ] && "$app/build.sh" >/dev/null 2>&1 && rm -rf "build/apps/$(basename "$app").app" && cp -R "$app/build/$(basename "$app").app" build/apps/
done
# JB=ssh (the default): sshd and a shell in the boot volume, root by the key in ssh/authorized_keys (git ignored); JB= for none
JB=${JB-ssh} BLUETOOTH=${BLUETOOTH:-1} BT_NAME="${BT_NAME:-Pixel 4a}" EXTRA_APPS="$PWD/build/apps" CRYPTEXES=flash FLASHBOOT=1 GL=1 REST=1 RC_JOBS=${RC_JOBS:-rc.jobs.daily} DENY_MORE="${DENY_MORE:-voicemail.vmd fairplayd fairplaydeviceidentityd}" userland/build.sh
