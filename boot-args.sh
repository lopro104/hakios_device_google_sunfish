# The kernel's boot arguments for the Pixel 4a, shared by run.sh (a run from the PC) and mkinstall.sh (the self-contained image).
# Sourced, not run. BOOT_ARGS_BASE: what every boot has; FLASH_ARGS: what a boot with the root in the darwin partition adds.
BOOT_ARGS_BASE="vtimer_intid=19 iowatchdog=1 wdt=-1 cpus=8 bootcpu=7 touchscreen=1 touch_delay=3 thermal=1 thermal_slow=80 panel_autorefresh=1 panel_brightness=100 gpu=1 cpufreq=1 pm6150_battery=1 mpss=1 wifi=1 wlanprobe=8 haptic=5"
FLASH_ARGS="root_apfs=ufs_darwin ufs=2 var_store=1 ufs_write=1"
