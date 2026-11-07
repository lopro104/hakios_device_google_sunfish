#!/system/bin/sh
# The module's Action button in the KernelSU manager: boot iOS, or say why not.
MODDIR=${0%/*}
exec sh $MODDIR/system/bin/ios-boot
