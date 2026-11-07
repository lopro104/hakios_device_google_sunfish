# Boot Switch

A small UIKit app: "Restart into Android" posts the Darwin notification `com.hakios.next-boot-android`, "Reboot" posts
`com.hakios.reboot` and `com.hakios.restart`. It makes no system calls of its own: a root daemon is to listen for them.
Written by Antigravity, read and kept by hand (nothing but `notify_post()`). `build.sh` makes build/BootSwitch.app.
Any process can post these notifications, so any app could restart the phone; nothing is lost by that.
