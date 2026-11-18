/*
 * The Pixel 4a's Bluetooth for Darwin's bluetoothd: an IOService named "bluetooth" (bluetoothd reads its public address
 * from it, "local-mac-address", in display order) with the controller's Supported Commands mask (the bluetoothd library
 * of userland/src/inject_bluetooth.c reads it) and "HakiosBluetoothReady" (the BlueTool stand-in answers by it). The H4
 * traffic itself goes through the kernel control com.hakios.bluetooth (hakios.bt_sunfish.c). After the Mi 10S port's
 * publisher (hakios-thyme, devices/thyme/drivers/bluetooth/hakios.thyme_bluetooth.cpp).
 */
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <sys/errno.h>

static IOService *bluetooth_service;

extern "C" void hakios_bt_sunfish_set_ready(bool ready);
extern "C" void
hakios_bt_sunfish_set_ready(bool ready)
{
	if (bluetooth_service != NULL) {
		bluetooth_service->setProperty("HakiosBluetoothReady", ready);
	}
}

extern "C" int hakios_bt_sunfish_publish(const uint8_t address[6], const uint8_t commands[64], uint32_t speed);
extern "C" int
hakios_bt_sunfish_publish(const uint8_t address[6], const uint8_t commands[64], uint32_t speed)
{
	IOService *platform = IOService::getPlatform();
	IOService *service = new IOService;
	OSData *mac, *supported;

	if (platform == NULL || service == NULL || !service->init()) {
		OSSafeReleaseNULL(service);
		return ENOMEM;
	}
	mac = OSData::withBytes(address, 6);
	supported = OSData::withBytes(commands, 64);
	if (mac == NULL || supported == NULL) {
		OSSafeReleaseNULL(mac);
		OSSafeReleaseNULL(supported);
		service->release();
		return ENOMEM;
	}
	service->setProperty("local-mac-address", mac);
	service->setProperty("HakiosBluetoothSupportedCommands", supported);
	mac->release();
	supported->release();
	service->setProperty("transport-speed", speed, 32);
	service->setProperty("compatible", "qcom,wcn3990-bt");
	service->setProperty("HakiosBluetoothReady", false);
	service->setName("bluetooth");
	if (!service->attach(platform)) {
		service->release();
		return EIO;
	}
	if (!service->start(platform)) {
		service->detach(platform);
		service->release();
		return EIO;
	}
	service->registerService();
	bluetooth_service = service;
	service->release();
	return 0;
}
