# Pixel 4a (sunfish) for HakiOS `common` (xnu-12377, Darwin 26/27)

The device package for upstream's new tree (oosexgang/hakios-xnu-kernel, branch `common`). The kernel with the
Pixel 4a's drivers is lopro104/hakios_kernel_google_sunfish, branch `sunfish-27`. Copy this directory into
`arch/arm64/boot/dts/sunfish/` of that checkout.

## State (iOS 27.2, 24B119, November 2026)

| What | State |
| --- | --- |
| Boot | From RAM: `fastboot boot` of U-Boot + the HakiOS loader, nothing flashed. Lock screen about 80 s after the kernel starts. |
| System | An Android DSU image (installed with Android's DSU, served by the loader as `ufs=2`; `init/stage0dsu.c`). The 4.29 GB system image does not fit the 3.87 GB RAM window, and `darwin` still holds the 16.5 install. |
| CPU | 8 cores, `bootcpu=7`, OSM frequency scaling (`cpufreq=1`: 1804 / 2208 MHz). |
| Display | Command-mode panel, `panel_autorefresh=1`; brightness slider with `backlight=2` (DCS 0x51, 10 bits, high byte first). |
| Touch, buttons, battery | `touchscreen=1 touch_delay=3 buttons=1 pm6150_battery=1` |
| Wi-Fi | `mpss=1 wlanprobe=8 mpss_no_shutdown=1`: scan, WPA2 join from Settings, DHCP, Safari, SSH. Firmware read from `darwin`'s fourth container (the 16.5 install's) on `/hakios/fw`. |
| GPU | `gpu=1 metal=1 metal_procs=backboardd`: the Adreno 618 and the Metal plug-in built `CHIP=618` (`HAKIOS_METAL_CHIP=618 darwin/userland/metal.sh`); backboardd draws with Metal, Liquid Glass included. SpringBoard and apps still draw in software. |
| Storage | `ufs_write=2 var_store=1 var_store_dev=dsudata var_store_keychain=1`: `/private/var` (settings, joined networks, the keychain, unencrypted) packed into the DSU install's `userdata_gsi` (2 GiB, two 1 GiB slots) every 5 minutes and unpacked at boot. Not into `darwin`: from 8 GiB on it is iOS 16.5's data volume. A DSU reinstall empties it. |
| Not yet | Flashlight, sound, camera, cellular, Bluetooth, the system in the phone's own partition. |

## How it was made

- `tools/boards.py new sunfish --soc sm7150 --dts phone.dts` from EvolutionX's dtb.img + dtbo.img (MP1.0 overlay
  applied with ufdt_apply_overlay), then filled in from the Darwin 16.5 port's measured values: loader fb-base
  0x9c000000, io window, GIC, PON on the PM6150; display clocks from msm-4.14's gcc/dispcc-sdmmagpie.c; OSM cpufreq
  domains.
- The kernel needs `vtimer_intid=19` (the SM7150's virtual timer PPI) and `panel_autorefresh=1`.

## Traps

- Stopping the modem (PAS shutdown) never returns: run with `mpss_no_shutdown=1`, and restart the phone (through
  Android) before the next run.
- A second GPU bring-up without a phone restart fails at step 16 (RSCC/PDC left programmed).
- The Metal plug-in's test kernels must be compiled for the A618 (`make air-fixtures fixtures CHIP_ID=0x06010800`
  with an LLVM 20 `llvm-as`; LLVM 23's bitcode is refused).
