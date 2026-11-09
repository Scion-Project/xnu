/*
 * iOS 27 on STARLTE (xnu-12377): what Scion's Galaxy S9 Wi-Fi, stage 2, asks of its kernel on this tree.
 *
 * The drivers are Scion's, taken from scion-kernel's drivers/ at build time (profiles/ios-27.copy): the PCIe link
 * (hakios.exynos_pcie.c), the kernel's side of Linux's brcmfmac (hakios.brcmfmac.c, hakios.lx.c; brcmfmac's own object
 * from tools/mk-brcmfmac.sh is linked in when the build is given it, HAKIOS_OBJECTS), en0 (hakios.net.c), the station
 * and the Apple80211 surface wifid talks to (hakios.sta.c, hakios.apple80211.c, hakios.wlan.cpp). On Scion's iOS 16.5
 * kernel they lean on parts this tree does not have; this file stands in for them:
 *
 *   hakios_image_file()      Scion reads the radio's files from fw/ of its small image. Here they are read from the iOS
 *                            root filesystem once it is mounted: /private/var/hakios/wifi/<name>, under bcmdhd's names
 *                            (bcmdhd_sta.bin_b2, bcmdhd_clm.blob, nvram.txt_*) and mac.info / network (the network to
 *                            join, two lines), as scion-starlte's WIFI.md names them. Nothing of the vendor's is in
 *                            any repository: the files are the phone's own (its VENDOR partition), put there by the
 *                            root's build. wifi_dir=<path> takes another directory; wifi_root_wait=<s> (default 120)
 *                            is how long a request waits for the root filesystem.
 *   hakios_partition_file()  Scion's ext4 reader of the VENDOR partition (ufs=2): not on this tree; nothing is found.
 *   gic_v3_spi_*()           the GIC calls of INTx (brcmfmac=5): not on this tree yet; attaching fails, so
 *                            hakios_wifi_option() makes brcmfmac_poll 1 unless given: the chip is polled.
 *   hakios_virtio_net_init() QEMU's virtio network link: none on the phone.
 *   starlte_wifi_init()      from IOKitBSDInit, where stage 1 was started: en0's options read (hakios_net_init()),
 *                            then the link (wifi_pcie=N; 6 hands the chip to brcmfmac, brcmfmac=N).
 *
 * Nothing runs without wifi_pcie=N: off by default.
 */
#include <stdint.h>
#include <stdbool.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/hakios.input.h>
#include <sys/hakios.xnu.h>

#define WIFI_DIR_DEFAULT        "/private/var/hakios/wifi/"
#define WIFI_FILE_LARGEST       (4u << 20)
#define WIFI_FILES_KEPT         12

static LCK_GRP_DECLARE(starlte_wifi_group, "starlte-wifi");
static LCK_MTX_DECLARE(files_lock, &starlte_wifi_group);

static struct {
	char name[64];
	uint8_t *data;
	uint64_t size;
} kept[WIFI_FILES_KEPT];

static int
root_file(const char *path, uint8_t **data, uint64_t *size)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	off_t len = 0;
	int e, resid = 0;
	uint8_t *buf;

	if ((e = vnode_open(path, FREAD, 0, 0, &vp, ctx)) != 0) {
		return e;
	}
	if (vnode_vtype(vp) != VREG) {
		e = EFTYPE;
	} else if ((e = vnode_size(vp, &len, ctx)) == 0 && (len <= 0 || (uint64_t)len > WIFI_FILE_LARGEST)) {
		e = EFBIG;
	}
	if (e == 0) {
		buf = kalloc_data((vm_size_t)len, Z_WAITOK | Z_ZERO);
		if (buf == NULL) {
			e = ENOMEM;
		} else if ((e = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)len, 0, UIO_SYSSPACE, 0, vfs_context_ucred(ctx),
		    &resid, current_proc())) != 0 || resid != 0) {
			kfree_data(buf, (vm_size_t)len);
			e = e != 0 ? e : EIO;
		} else {
			*data = buf;
			*size = (uint64_t)len;
		}
	}
	(void)vnode_close(vp, FREAD, ctx);
	return e;
}

/* the root filesystem, waited for at most wifi_root_wait seconds (a request may come from stage 1's thread before it) */
static bool
root_there(void)
{
	static bool said;
	uint32_t wait_s = 120;

	PE_parse_boot_argn("wifi_root_wait", &wait_s, sizeof(wait_s));
	for (uint32_t waited = 0; rootvnode == NULLVP && waited < wait_s * 4; waited++) {
		if (!said) {
			said = true;
			printf("wifi: the radio's files wait for the root filesystem (wifi_root_wait=%u s)\n", wait_s);
		}
		IOSleep(250);
	}
	return rootvnode != NULLVP;
}

/*
 * Scion's fw/ of the small image, here a directory of the root filesystem. Only dir "fw" is asked for (hakios.brcmfmac.c).
 * A file read once is kept (brcmfmac may ask again after a reset); what is not there is NULL at once.
 */
const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	char base[96] = WIFI_DIR_DEFAULT, path[192];
	const uint8_t *found = NULL;
	uint8_t *data = NULL, *result;
	uint64_t length = 0;
	int e;

	if (disk_out != NULL) {
		*disk_out = 0;
	}
	if (dir == NULL || strcmp(dir, "fw") != 0 || name == NULL || name[0] == 0 || strchr(name, '/') != NULL ||
	    strlen(name) >= sizeof(kept[0].name)) {
		return NULL;
	}
	lck_mtx_lock(&files_lock);
	for (unsigned i = 0; i < WIFI_FILES_KEPT; i++) {
		if (kept[i].data != NULL && strcmp(kept[i].name, name) == 0) {
			*size = kept[i].size;
			found = kept[i].data;
			break;
		}
	}
	lck_mtx_unlock(&files_lock);
	if (found != NULL) {
		return found;
	}
	if (!root_there()) {
		printf("wifi: %s: no root filesystem\n", name);
		return NULL;
	}
	PE_parse_boot_arg_str("wifi_dir", base, sizeof(base));
	snprintf(path, sizeof(path), "%s%s%s", base, base[0] != 0 && base[strlen(base) - 1] == '/' ? "" : "/", name);
	if ((e = root_file(path, &data, &length)) != 0) {
		printf("wifi: %s: not read (errno %d)\n", path, e);
		return NULL;
	}
	printf("wifi: %s: %llu bytes\n", path, length);
	result = data;
	lck_mtx_lock(&files_lock);
	for (unsigned i = 0; i < WIFI_FILES_KEPT; i++) {
		if (kept[i].data == NULL) {
			strlcpy(kept[i].name, name, sizeof(kept[i].name));
			kept[i].size = length;
			kept[i].data = data;
			data = NULL;
			break;
		}
	}
	lck_mtx_unlock(&files_lock);
	if (data != NULL) {
		/* the table is full (it is not, with brcmfmac's handful of files): the caller still gets it, kept for good */
		printf("wifi: %s: kept outside the table\n", name);
	}
	*size = length;
	return result;
}

/* Scion's VENDOR reader (ext4 on UFS): not on this tree */
int hakios_partition_file(const char *partition, const char *mount, const char *path, uint64_t max, const uint8_t **data,
    uint64_t *size);
int
hakios_partition_file(__unused const char *partition, __unused const char *mount, __unused const char *path,
    __unused uint64_t max, __unused const uint8_t **data, __unused uint64_t *size)
{
	return ENOENT;
}

/* INTx through the GIC (brcmfmac=5): not on this tree yet; the driver polls */
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
kern_return_t
gic_v3_spi_attach(uint32_t intid, __unused bool edge, __unused void (*handler)(void *context, uint32_t intid),
    __unused void *context)
{
	printf("wifi: SPI %u: no GIC attach on this kernel: the radio is polled\n", intid);
	return KERN_NOT_SUPPORTED;
}
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_mask(__unused uint32_t intid) {}
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_unmask(__unused uint32_t intid) {}
void gic_v3_spi_detach(uint32_t intid);
void gic_v3_spi_detach(__unused uint32_t intid) {}
uint64_t gic_v3_spi_count(uint32_t intid);
uint64_t gic_v3_spi_count(__unused uint32_t intid) { return 0; }

/*
 * The radio driver's options (hakios.brcmfmac.c has a weak one): as there, the boot-arg of the name; but brcmfmac_poll is 1
 * unless given, as INTx (brcmfmac=5 and up) has no GIC attach on this tree: the chip is polled every brcmfmac_poll_ms.
 */
uint32_t hakios_wifi_option(const char *name);
uint32_t
hakios_wifi_option(const char *name)
{
	uint32_t value = strcmp(name, "brcmfmac_poll") == 0 ? 1 : 0;

	PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

/* QEMU's virtio network device: not on the phone */
void hakios_virtio_net_init(void);
void hakios_virtio_net_init(void) {}

void hakios_net_init(void);
void starlte_wifi_init(void);
void
starlte_wifi_init(void)
{
	uint32_t steps = 0;

	if (!PE_parse_boot_argn("wifi_pcie", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	hakios_net_init();
	hakios_exynos_pcie_init();
}
