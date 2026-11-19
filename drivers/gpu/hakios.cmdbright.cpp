/*
 * AppleARMBacklight, a stand-in: CoreBrightness ([CBDisplayContaineriOS findBacklight]) matches IOPropertyMatch
 * {"backlight-control" = true}, then this class, and reads its IODisplayParameters "brightness" {min, max, value}. Without
 * one iOS's brightness slider stays dead. A value it sets goes to the panel (hakios.cmdbright.c).
 *
 * The slider is linear and the panel's level is linear in light, so a straight map puts all the visible change at the bottom
 * of the slider: gamma 2.2 instead, 4 + 1019 * (value / 1024)^2.2, in 16 interpolated steps.
 *
 * From SweetFramebuffer.cpp of ios-sunfish, which ran on this phone (2026-10-28). Status: RUN ON THE PHONE, iOS 16.5
 * (2026-11-05): the user moved the slider and the panel followed.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>

#define SLIDER_MAX      1024

#include <pexpert/pexpert.h>
extern "C" int hakios_panel_brightness(int level);
extern "C" bool hakios_panel_follows_power(void);
extern "C" void HakiosBacklightStart(int level);

static const uint16_t curve[17] = { 4, 6, 15, 30, 52, 83, 122, 169, 226, 291, 366, 451, 545, 649, 764, 888, 1023 };

static int
level_of(uint32_t value)
{
	uint32_t i = value / 64, f = value % 64;

	return i >= 16 ? curve[16] : curve[i] + (int)((curve[i + 1] - curve[i]) * f / 64);
}

static uint32_t
value_of(int level)
{
	for (uint32_t i = 0; i < 16; i++) {
		if (level <= curve[i + 1]) {
			return i * 64 + (uint32_t)(level > curve[i] ? (level - curve[i]) * 64 / (curve[i + 1] - curve[i]) : 0);
		}
	}
	return SLIDER_MAX;
}

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);
public:
	uint32_t fValue;
	void publish(void);
	IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

void
AppleARMBacklight::publish(void)
{
	OSDictionary * params = OSDictionary::withCapacity(1);
	OSDictionary * brightness = OSDictionary::withCapacity(3);

	if (params != NULL && brightness != NULL) {
		OSNumber * n = OSNumber::withNumber(0ULL, 32);

		brightness->setObject("min", n);
		OSSafeReleaseNULL(n);
		n = OSNumber::withNumber((unsigned long long) SLIDER_MAX, 32);
		brightness->setObject("max", n);
		OSSafeReleaseNULL(n);
		n = OSNumber::withNumber((unsigned long long) fValue, 32);
		brightness->setObject("value", n);
		OSSafeReleaseNULL(n);
		params->setObject("brightness", brightness);
		setProperty("IODisplayParameters", params);
	}
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(params);
}

static bool
number_of(OSObject * o, uint32_t * out)
{
	OSNumber * n = OSDynamicCast(OSNumber, o);
	OSDictionary * d = OSDynamicCast(OSDictionary, o);

	if (n == NULL && d != NULL) {
		n = OSDynamicCast(OSNumber, d->getObject("value"));
	}
	if (n == NULL) {
		return false;
	}
	*out = n->unsigned32BitValue();
	return true;
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * d = OSDynamicCast(OSDictionary, properties);
	uint32_t value;

	if (d != NULL) {
		OSDictionary * params = OSDynamicCast(OSDictionary, d->getObject("IODisplayParameters"));

		if (number_of(params ? params->getObject("brightness") : d->getObject("brightness"), &value)) {
			fValue = value > SLIDER_MAX ? SLIDER_MAX : value;
			/* 0 comes just before iOS powers the display off, and again just before it powers it on: with the panel
			 * following iOS's display power, not sent (it showed as a dim step before the screen went off) */
			if (fValue != 0 || !hakios_panel_follows_power()) {
				hakios_panel_brightness(level_of(fValue));
			}
			publish();
		}
	}
	return kIOReturnSuccess;
}

/*
 * For Settings' native Auto-Brightness switch: MobileGestalt's "ambient-light-sensor" is whether an IOService with
 * PrimaryUsagePage 0xff00 and PrimaryUsage 4 exists (Evsio0n/instantnoodle-xnu, 20F66). A plain service, not a HID one:
 * CoreBrightness, which takes down backboardd on a light sensor of ours, looks only at HID services. The light itself
 * and the curve: hakios.cmdbright.c (with boot-arg sns=2, the sensors).
 */
class HakiosLightSensor : public IOService
{
	OSDeclareDefaultStructors(HakiosLightSensor);
};

OSDefineMetaClassAndStructors(HakiosLightSensor, IOService);

static void
light_sensor_publish(IOService * platform)
{
	HakiosLightSensor * sensor = OSTypeAlloc(HakiosLightSensor);
	uint32_t sns = 0;

	if (!PE_parse_boot_argn("sns", &sns, sizeof(sns)) || sns < 2 || sensor == NULL || !sensor->init()) {
		OSSafeReleaseNULL(sensor);
		return;
	}
	sensor->setProperty("PrimaryUsagePage", 0xff00ULL, 32);
	sensor->setProperty("PrimaryUsage", 4ULL, 32);
	sensor->setName("als");
	if (sensor->attach(platform)) {
		sensor->registerService();
		IOLog("cmdbright: light sensor service published: Settings' Auto-Brightness switch\n");
	}
	OSSafeReleaseNULL(sensor);
}

void
HakiosBacklightStart(int level)
{
	AppleARMBacklight * light = OSTypeAlloc(AppleARMBacklight);
	IOService * platform = IOService::getPlatform();

	if (light == NULL || platform == NULL || !light->init()) {
		IOLog("cmdbright: no brightness slider: no platform yet\n");
		OSSafeReleaseNULL(light);
		return;
	}
	light->fValue = value_of(level);
	light->publish();
	light->setProperty("backlight-control", kOSBooleanTrue);
	light->setName("backlight");
	if (light->attach(platform)) {
		light->registerService();
		IOLog("cmdbright: AppleARMBacklight published: the brightness slider\n");
	}
	light_sensor_publish(platform);
	OSSafeReleaseNULL(light);
}
