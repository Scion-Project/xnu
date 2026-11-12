/*
 * STARLTE (Galaxy S9): firmware files that travel with the kernel, for the drivers that load a chip's RAM image at
 * boot, before there is a root and without asking it.
 *
 * Scion's shared drivers ask for such a file with hakios_image_file("fw", NAME, &size, NULL) (sys/hakios.input.h).
 * On the trees with a RAM disk beside the kernel the answer comes out of that disk's fw/ directory. This tree's boot
 * image has no RAM disk; its loader hands the kernel one device tree, and the tool that packs the image may put a
 * node "/scion-firmware" into it with one property for each file: the property's name is the file's name (at most 31
 * characters, the device tree's limit), its value the file's bytes. The flat tree stays where the loader put it for
 * as long as the phone runs (its memory map names no "DeviceTree" region to give back), so the bytes are used where
 * they lie.
 *
 * The files themselves are the chips' makers' and are in no repository of ours; the packing step takes them from the
 * builder's own copy. First user: the sensor hub's RAM image (bsd/dev/hakios.ssp.c, boot-arg shub=4 and up).
 *
 * SPDX-License-Identifier: MIT-0
 */
#include <libkern/libkern.h>
#include <pexpert/device_tree.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

#define FIRMWARE_NODE   "/scion-firmware"

const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	DTEntry node;
	const void *bytes = NULL;
	unsigned int length = 0;

	if (size != NULL) {
		*size = 0;
	}
	if (disk_out != NULL) {
		*disk_out = -1;
	}
	/* only fw/ is carried this way */
	if (dir == NULL || name == NULL || strcmp(dir, "fw") != 0) {
		return NULL;
	}
	if (SecureDTLookupEntry(NULL, FIRMWARE_NODE, &node) != kSuccess ||
	    SecureDTGetProperty(node, name, &bytes, &length) != kSuccess || bytes == NULL || length == 0) {
		return NULL;
	}
	if (size != NULL) {
		*size = length;
	}
	return (const uint8_t *)bytes;
}
