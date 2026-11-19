/*
 * The USB host as lockdownd sees it, for the usbmux interface of drivers/umi/hakios.usb.c (usb_ncm=1).
 *
 * lockdownd asks "Trust This Computer?" only while it believes a USB host is attached; otherwise
 * ask_user_to_trust logs "USB or proxy host no longer connected." and the pair fails (InvalidHostID,
 * or PasswordProtected for a host that does not ask for ExtendedPairingErrors). Its notification_worker
 * learns that from IOKit, as Apple's USB device stack tells it (iOS 16.5 lockdownd, disassembled):
 *  - on the first match of an AppleUSBDeviceMux service it reads every IOUSBDeviceInterface, and one
 *    whose USBDeviceFunction is "AppleUSBMux" and IsActive is true means a host is attached now
 *    (detect_connected_usb_host -> usb_host_connected(mux));
 *  - then it listens to the AppleUSBDeviceMux service's general interest: message 0xe3ff8201 with a
 *    non-zero argument says a host attached (HostAttached, com.apple.mobile.lockdown.host_attached),
 *    with zero that it left (mux_notification_handler).
 * These two services say exactly that, following the gadget's configuration. Nothing opens them.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>

#define kHakiosUSBMuxHostConnection 0xe3ff8201  /* lockdownd's mux_notification_handler */

class IOUSBDeviceInterface : public IOService
{
	OSDeclareDefaultStructors(IOUSBDeviceInterface);
};
OSDefineMetaClassAndStructors(IOUSBDeviceInterface, IOService);

class AppleUSBDeviceMux : public IOService
{
	OSDeclareDefaultStructors(AppleUSBDeviceMux);
};
OSDefineMetaClassAndStructors(AppleUSBDeviceMux, IOService);

extern "C" void hakios_usbmux_host(int connected);

static IOUSBDeviceInterface * interface;
static AppleUSBDeviceMux * mux;

/* From the USB worker whenever the host configures the gadget (1) or loses it (0); the first call (1) makes the services. */
void
hakios_usbmux_host(int connected)
{
	if (mux == NULL) {
		IOService * platform = IOService::getPlatform();

		interface = OSTypeAlloc(IOUSBDeviceInterface);
		mux = OSTypeAlloc(AppleUSBDeviceMux);
		if (platform == NULL || interface == NULL || mux == NULL || !interface->init() || !mux->init()) {
			OSSafeReleaseNULL(interface);
			OSSafeReleaseNULL(mux);
			IOLog("[usb-mux] cannot make the IOKit host services\n");
			return;
		}
		interface->setProperty("USBDeviceFunction", "AppleUSBMux");
		interface->setProperty("IsActive", connected != 0);
		interface->attach(platform);
		/* (matched before the mux appears: lockdownd reads the interfaces on the mux's first match) */
		interface->registerService(kIOServiceSynchronous);
		mux->attach(interface);
		mux->registerService();
		IOLog("[usb-mux] IOKit: IOUSBDeviceInterface AppleUSBMux IsActive=%d, AppleUSBDeviceMux published\n", connected != 0);
		return;
	}
	interface->setProperty("IsActive", connected != 0);
	mux->messageClients(kHakiosUSBMuxHostConnection, (void *)(uintptr_t)(connected != 0));
	IOLog("[usb-mux] IOKit: host %s\n", connected ? "attached" : "detached");
}
