#!/bin/bash
# The RAM part of a boot from the darwin partition on the Pixel 4a: flashboot.apfs (4 MB) and root.apfs (with Mesa), made with
# the everyday job list. Run from ~/hakios:   devices/sunfish/build-flash.sh      (run it with devices/sunfish/run.sh, FLASH=1)
# DENY_MORE takes out jobs that only cost heat here: Visual Voicemail (no baseband) and FairPlay (restarts every 10 s).
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
CRYPTEXES=flash FLASHBOOT=1 GL=1 REST=1 RC_JOBS=${RC_JOBS:-rc.jobs.daily} DENY_MORE="${DENY_MORE:-voicemail.vmd fairplayd fairplaydeviceidentityd}" userland/build.sh
