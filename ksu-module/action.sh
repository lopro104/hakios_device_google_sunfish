#!/system/bin/sh
# The module's Action button in the KernelSU manager: show where things stand, then boot iOS if it is ready (the slot chosen, the
# image installed there), or say what is missing. It never installs or flashes anything.
MODDIR=${0%/*}
IOSBOOT=$MODDIR/system/bin/ios-boot
echo "== HakiOS iOS =="
sh $IOSBOOT status
echo
sh $IOSBOOT
