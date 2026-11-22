# Pixel 4a (sunfish) for HakiOS `common` (xnu-12377, Darwin 26/27)

The device package for upstream's new tree (github.com/oosexgang/hakios-xnu-kernel, branch `common`; our work on
it: branch `sunfish26` of ~/hakios26). Copy into `arch/arm64/boot/dts/sunfish/` of that checkout.

- Made by `tools/boards.py new sunfish --soc sm7150 --dts phone.dts` from EvolutionX's dtb.img + dtbo.img (MP1.0
  overlay applied with ufdt_apply_overlay), then filled in from the Darwin 16.5 port's measured values: loader fb-base
  0x9c000000, io window, GIC, cores 0-5 small; PON on the PM6150; display clocks from msm-4.14's gcc/dispcc-sdmmagpie.c.
- Needs the kernel's vtimer_intid=19 and cmdpanel (panel_autorefresh=1), both on sunfish26.
- NOT RUN ON THE PHONE YET. The system image (4.29 GB) does not fit the 3.87 GB RAM window: it has to come from flash
  (the DSU path: stage0dsu, `oem dsu`), since `darwin` is full with the 16.5 install.
