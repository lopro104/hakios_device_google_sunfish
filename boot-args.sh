# The kernel's boot arguments for the Pixel 4a, shared by run.sh (a run from the PC) and mkinstall.sh (the self-contained image).
# Sourced, not run. BOOT_ARGS_BASE: what every boot has; FLASH_ARGS: what a boot with the root in the darwin partition adds.
# The rest of what every boot has is built into the kernel: CONFIG_DEFAULT_ARGS in configs/sunfish.config. Only what the
# loader itself reads stays here (it makes the cores and the flashlight of the device tree from these).
BOOT_ARGS_BASE="cpus=8 bootcpu=7 torch=3 cam=5"   # cam=5: the cameras (powered only while used; the loader gives Darwin the ISP node for it)
FLASH_ARGS="root_apfs=ufs_darwin ufs=2 ufs_write=1 var_hfs=ufs_darwin_data"
