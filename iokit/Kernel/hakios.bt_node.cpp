/*
 * Starbite iOS 27 (xnu-12377) port: Scion-Project/scion-kernel drivers/bluetooth/bcm4361/hakios.bt_node.cpp (APSL;
 * HakiOS/Scion), unchanged but for this note; here in iokit/Kernel/, called by bsd/dev/hakios.bcm4361_bt.c at bt=7.
 * What iOS 27's bluetoothd/BlueTool look for has not been checked (the text below is from iOS 16.5's).
 */
/*
 * HakiosBluetooth: the registry entry by which iOS's bluetoothd finds the S9's Bluetooth (drivers/bluetooth/
 * hakios.bcm4361_bt.c, bt=7).
 *
 * bluetoothd 16.5 (iPhone X build) looks for a service named "bluetooth" (the device tree's node on an iPhone) and
 * takes the controller's address from its property local-mac-address; with none, or all zeros, it says "Not a valid
 * MAC address (all 00s)" and "No bluetooth on this device." and exits (seen on the phone, 3 November). Its transport
 * it takes from BT_HCI_TRANSPORT (H4:POSIX:com.apple.uart.bluetooth,4000000, set by the job: userland's BT=1), which
 * opens the C driver's kernel control. BlueTool, through which bluetoothd runs its boot and init scripts first, looks
 * the same service up: "device -D -S" opens the control at transport-speed (when the variable BT_BAUDRATE does not say
 * otherwise), and transport-encoding, absent here, would make it AppleBT (6) or PCIe (7) instead of a UART (any other
 * value, 8 when there is none). So: the node's own service if the device tree has one, else ours, named "bluetooth",
 * under the platform, with the chip's address (in the order it is written, not HCI's) and transport-speed, the UART's
 * rate.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>

extern "C" void HakiosBTNodeStart(const uint8_t * hci_address, uint32_t speed);

class HakiosBluetooth : public IOService
{
	OSDeclareDefaultStructors(HakiosBluetooth);
};

OSDefineMetaClassAndStructors(HakiosBluetooth, IOService);

/* Once, when the chip is up, patched and its address read. */
void
HakiosBTNodeStart(const uint8_t * hci_address, uint32_t speed)
{
	IOService * platform = IOService::getPlatform();
	OSDictionary * match = IOService::nameMatching("bluetooth");
	IOService * node = match != NULL ? IOService::copyMatchingService(match) : NULL;
	uint8_t mac[6];
	OSData * data;
	bool ours = false;

	OSSafeReleaseNULL(match);
	for (int i = 0; i < 6; i++) {
		mac[i] = hci_address[5 - i];
	}
	if (node == NULL) {
		HakiosBluetooth * service = OSTypeAlloc(HakiosBluetooth);

		if (platform == NULL || service == NULL || !service->init() || !service->attach(platform)) {
			OSSafeReleaseNULL(service);
			IOLog("HakiosBluetooth: cannot make the \"bluetooth\" service\n");
			return;
		}
		service->setName("bluetooth");
		node = service;
		ours = true;
	}
	if ((data = OSData::withBytes(mac, sizeof(mac))) != NULL) {
		node->setProperty("local-mac-address", data);
		data->release();
	}
	if ((data = OSData::withBytes(&speed, sizeof(speed))) != NULL) {
		node->setProperty("transport-speed", data);
		data->release();
	}
	if (ours) {
		if (!node->start(platform)) {
			node->detach(platform);
			node->release();
			IOLog("HakiosBluetooth: the \"bluetooth\" service did not start\n");
			return;
		}
		node->registerService();
	}
	IOLog("HakiosBluetooth: %s \"bluetooth\": local-mac-address %02x:%02x:%02x:%02x:%02x:%02x, transport-speed %u\n",
	    ours ? "our" : "the device tree's", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], speed);
	node->release();
}
