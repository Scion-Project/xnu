/*
 * The whole disk as the IORegistry shows it on the S9 ("disk0").
 *
 * This kernel has no IOStorageFamily, so no IOMedia exists (bsd/dev/herolte_ufs.c exposes the root as
 * a devfs node only). iOS 27's libMobileGestalt wants one: its NANDSize() does
 *     IOServiceGetMatchingService(kIOMasterPortDefault, IOBSDNameMatching(kIOMasterPortDefault, 0, "disk0"))
 *     IORegistryEntryCreateCFProperty(disk0, CFSTR("Size"), kCFAllocatorDefault, 0)    (kIOMediaSizeKey, a CFNumber)
 * and answers "DiskUsage"/TotalDiskCapacity from it. Without the node the answer is -1 and Settings >
 * General > About shows "Capacity: 0 KB" (iPhone Storage, which asks the same, too).
 *
 * herolte_ufs.c calls sweet_media_publish() when it has found the root partition, with the size of the
 * UFS LUN the GPT lies on (64 GB phone: about 64 * 10^9 bytes; About rounds it to whole GB). The node is
 * a plain IOService under the platform expert with "BSD Name" = disk0, "Size" and "Whole". It is not an
 * IOMedia: DiskArbitration and the IOStorage matching never see it.
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

extern "C" void sweet_media_publish(uint64_t bytes);

void
sweet_media_publish(uint64_t bytes)
{
	static bool published;
	IOService *platform = IOService::getPlatform();
	IOService *disk;

	if (published || bytes == 0 || platform == NULL) {
		return;
	}
	disk = new IOService;
	if (disk == NULL) {
		return;
	}
	if (!disk->init()) {
		disk->release();
		return;
	}
	disk->setName("disk0");
	disk->setProperty("BSD Name", "disk0");
	disk->setProperty("Size", bytes, 64);
	disk->setProperty("Whole", true);
	disk->setProperty("Writable", true);
	disk->setProperty("Removable", false);
	if (!disk->attach(platform)) {
		disk->release();
		return;
	}
	disk->registerService();
	disk->release();        /* the registry (the attach) holds it */
	published = true;
	IOLog("SWEET MEDIA: disk0 registered, Size %llu\n", bytes);
}
#endif /* ARM64_BOARD_CONFIG_HEROLTE */
