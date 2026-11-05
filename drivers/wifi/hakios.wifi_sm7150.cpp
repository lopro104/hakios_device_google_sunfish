/*
 * IO80211Interface stand-in for the Wi-Fi interface (bsd/dev/sweet_mpss.c).
 *
 * SystemConfiguration's SCNetworkInterfaceCopyAll looks for services matching
 * "IONetworkInterface", and checks if their parent conforms to "IO80211Controller"
 * (or AirPortPCI / AirPortDriver). When both match, SC assigns type "IEEE80211".
 *
 * Apple's IO80211 library (Apple80211GetIfListCopy / Apple80211BindToInterface,
 * used by wifid, MobileWiFi, and Settings) looks up the IORegistry entry with
 * IOServiceGetMatchingService({IOPropertyMatch: {IOInterfaceName: ifname}}),
 * reading "IO80211InterfaceRole" ("Infrastructure").
 */
#include <device/device_types.h> /* io_buf_ptr_t for private OSKext headers */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

class IO80211Controller : public IOService {
	OSDeclareDefaultStructors(IO80211Controller);
};
OSDefineMetaClassAndStructors(IO80211Controller, IOService);

class IONetworkInterface : public IOService {
	OSDeclareDefaultStructors(IONetworkInterface);
};
OSDefineMetaClassAndStructors(IONetworkInterface, IOService);

class IOEthernetInterface : public IONetworkInterface {
	OSDeclareDefaultStructors(IOEthernetInterface);
};
OSDefineMetaClassAndStructors(IOEthernetInterface, IONetworkInterface);

class IO80211Interface : public IOEthernetInterface {
	OSDeclareDefaultStructors(IO80211Interface);
};
OSDefineMetaClassAndStructors(IO80211Interface, IOEthernetInterface);

extern "C" void
sweet_io80211_publish(const char *ifname, const uint8_t mac[6])
{
	IO80211Controller *ctrl = OSTypeAlloc(IO80211Controller);
	if (ctrl != NULL && ctrl->init()) {
		ctrl->setName("wlan");
		ctrl->attach(IOService::getPlatform());
		ctrl->registerService();
	}

	IO80211Interface *iface = OSTypeAlloc(IO80211Interface);
	OSNumber *n;
	OSData *d;

	if (iface == NULL || !iface->init()) {
		OSSafeReleaseNULL(iface);
		OSSafeReleaseNULL(ctrl);
		return;
	}
	iface->setName(ifname);
	iface->setProperty("IOInterfaceName", ifname);
	iface->setProperty("BSD Name", ifname);
	iface->setProperty("IO80211InterfaceRole", "Infrastructure");
	iface->setProperty("IOInterfaceNamePrefix", "en");
	n = OSNumber::withNumber(0ULL, 32);
	iface->setProperty("IOInterfaceUnit", n);
	OSSafeReleaseNULL(n);
	n = OSNumber::withNumber(6ULL, 32);                     /* IFT_ETHER */
	iface->setProperty("IOInterfaceType", n);
	OSSafeReleaseNULL(n);
	d = OSData::withBytes(mac, 6);
	iface->setProperty("IOMACAddress", d);
	OSSafeReleaseNULL(d);
	iface->setProperty("IOPrimaryInterface", kOSBooleanTrue);

	if (ctrl != NULL && iface->attach(ctrl)) {
		iface->registerService();
		IOLog("SWEET WIFI: IO80211Interface published for %s attached to IO80211Controller\n", ifname);
	} else if (iface->attach(IOService::getPlatform())) {
		iface->registerService();
		IOLog("SWEET WIFI: IO80211Interface published for %s attached to platform\n", ifname);
	}
	OSSafeReleaseNULL(iface);
	OSSafeReleaseNULL(ctrl);
}
