/*
 * iOS 27 on STARLTE: the network a test image is given to join (bsd/dev/starlte_wifi.c reads /chosen "hakios-wifi-network",
 * put there by scion-kernel tools/ios27/wifi/adt-wifi-network.py) is taken out of the registry's device-tree plane once the
 * kernel has its copy, so that no process can read the passphrase with IORegistry calls. The kernel's own device tree
 * memory still has it; only an image made for one's own phone carries it.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>

extern "C" void starlte_wifi_dt_forget(void);

void
starlte_wifi_dt_forget(void)
{
	IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);

	if (chosen != NULL) {
		chosen->removeProperty("hakios-wifi-network");
		chosen->release();
	}
}
