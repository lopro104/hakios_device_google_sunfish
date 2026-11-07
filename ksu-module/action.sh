#!/system/bin/sh
# The module's Action button in the KernelSU manager. Volume keys answer; nothing is flashed here.
#   1. if no iOS slot is chosen: Volume UP chooses the slot opposite the one Android runs from (recommended), Volume DOWN does nothing
#   2. then: Volume UP boots iOS (its slot made active, reboot), Volume DOWN stays on Android. No key within 20 seconds: nothing happens.
# The iOS image itself is put in its slot by hand, once:  ios-boot install IMAGE  (it asks, backs up, reads back).
MODDIR=${0%/*}
IOSBOOT=$MODDIR/system/bin/ios-boot
CONF=/data/adb/hakios_ios_slot

cur=$(getprop ro.boot.slot_suffix); cur=${cur#_}
case "$cur" in a) opp=b ;; b) opp=a ;; *) echo "cannot tell which slot Android runs from"; exit 1 ;; esac

# wait up to 20 s for a volume key: prints up or down, nothing on a timeout
key() {
	timeout 20 getevent -ql 2>/dev/null | while read -r _ _ code value; do
		case "$code $value" in
			"KEY_VOLUMEUP DOWN") echo up; break ;;
			"KEY_VOLUMEDOWN DOWN") echo down; break ;;
		esac
	done
}

echo "== HakiOS iOS =="
sh $IOSBOOT status
echo

ios=$(cat $CONF 2>/dev/null)
if [ -z "$ios" ]; then
	echo "No iOS slot chosen. Android runs from slot $cur."
	echo "  Volume UP:   iOS gets slot $opp   <- recommended (the other one)"
	echo "  Volume DOWN: do nothing"
	k=$(key)
	if [ "$k" = up ]; then
		sh $IOSBOOT set-slot $opp --yes || exit 1
		ios=$opp
		echo
	else
		echo "nothing changed"
		exit 0
	fi
fi

if [ "$ios" = "$opp" ]; then rec="iOS"; else rec="none (the iOS slot is the one Android runs from)"; fi
echo "Boot which? Android runs from slot $cur; iOS has slot $ios."
echo "  Volume UP:   iOS (slot $ios)   <- recommended: $rec"
echo "  Volume DOWN: stay on Android (slot $cur)"
k=$(key)
case "$k" in
	up) sh $IOSBOOT ;;
	down) echo "staying on Android" ;;
	*) echo "no key pressed: nothing happened" ;;
esac
