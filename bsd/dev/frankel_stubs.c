/*
 * Copyright (c) 2026 Scion Project. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * FRANKEL (Google Pixel 10, Tensor G5 "lga"): the S9's layer is built for this board too, and some of its files that
 * are compiled for every generic board call into the S9's own drivers (Exynos USB console, Mali-G72, PMIC over ACPM,
 * the S9's UFS root) or into SWEET's Qualcomm ones, which this tree does not carry. Each of those entry points is
 * answered here, on FRANKEL only (config option scion_frankel), by "not here": nothing below touches hardware.
 * In particular the PMIC calls refuse: no PMIC register of the Pixel is ever read or written by this kernel.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <pexpert/pexpert.h>

/* the kernel log ring the S9 keeps for its UFS/SD log (osfmk/console/serial_console.c, starlte_usbcon.c) */
void sweet_log_append(const char *c, int size);
void sweet_log_append(const char *c, int size) { (void)c; (void)size; }
void a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total);
void
a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total)
{
	*ring = NULL; *size = 0; *total = 0;
}

/* the S9's USB console verbs (hakios.dwc3.c) */
void hakios_input_console_button(const char *arg);
void hakios_input_console_button(const char *arg) { (void)arg; }
void hakios_input_console_touch(const char *arg);
void hakios_input_console_touch(const char *arg) { (void)arg; }
void hakios_exynos_cpufreq_touch(void);
void hakios_exynos_cpufreq_touch(void) { }

/*
 * The root on a UFS partition (IOKitBSDInit.cpp, rd=ufsroot0): Scion's own UFS driver (bsd/dev/hakios.ufs.c, hakios.ufshc.c:
 * read only on this board, ufs_native=4 ufs_native_soc=lga, or lga-qemu in QEMU) brought up here, and the partition
 * ufs_root=NAME (default "userdata") is the root. hakios_ufs_read_partition() is that driver's.
 */
extern void hakios_ufs_init(void);
extern int hakios_ufs_dev_by_name(const char *name, dev_t *dev);
int sweet_ufs_rootdev(dev_t *dev);
int
sweet_ufs_rootdev(dev_t *dev)
{
	static int started;
	char name[36] = "userdata";

	if (!started) {
		started = 1;
		hakios_ufs_init();
	}
	(void)PE_parse_boot_argn("ufs_root", name, sizeof(name));
	int error = hakios_ufs_dev_by_name(name, dev);
	printf("FRANKEL: root on the UFS partition %s: %s (%d)\n", name, error == 0 ? "found" : "not there", error);
	return error;
}

/* the S9's Mali-G72 (StarbiteMali.cpp): no GPU on this board yet (the Pixel's is a PowerVR) */
int starlte_gpu_user_ready(void);
int starlte_gpu_user_ready(void) { return 0; }
bool starlte_gpu_user_param(uint32_t param, uint64_t *value);
bool starlte_gpu_user_param(uint32_t param, uint64_t *value) { (void)param; (void)value; return false; }
bool starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n);
bool starlte_gpu_user_map(uint64_t va, const uint64_t *pages, unsigned n) { (void)va; (void)pages; (void)n; return false; }
void starlte_gpu_user_unmap(uint64_t va, unsigned n);
void starlte_gpu_user_unmap(uint64_t va, unsigned n) { (void)va; (void)n; }
int starlte_gpu_user_submit(uint64_t jc, uint32_t requirements);
int starlte_gpu_user_submit(uint64_t jc, uint32_t requirements) { (void)jc; (void)requirements; return ENODEV; }
int starlte_gpu_user_submit2(uint64_t jc, uint32_t requirements, bool async, uint64_t *seq);
int
starlte_gpu_user_submit2(uint64_t jc, uint32_t requirements, bool async, uint64_t *seq)
{
	(void)jc; (void)requirements; (void)async;
	if (seq) {
		*seq = 0;
	}
	return ENODEV;
}
int starlte_gpu_user_wait_seq(uint64_t seq, bool block);
int starlte_gpu_user_wait_seq(uint64_t seq, bool block) { (void)seq; (void)block; return 0; }
int starlte_gpu_user_wait_seq_for(uint64_t seq, uint64_t timeout_ns, uint64_t *done);
int
starlte_gpu_user_wait_seq_for(uint64_t seq, uint64_t timeout_ns, uint64_t *done)
{
	(void)seq; (void)timeout_ns;
	if (done) {
		*done = 0;
	}
	return 0;
}
uint64_t starlte_gpu_user_seq(void);
uint64_t starlte_gpu_user_seq(void) { return 0; }

/* the S9's PMIC over ACPM (starlte_usbcon.c): refused. No PMIC access on this board, read or write. */
bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
bool starlte_pmic_read(uint32_t type, uint32_t reg, uint8_t *val) { (void)type; (void)reg; (void)val; return false; }
bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
bool starlte_pmic_write(uint32_t type, uint32_t reg, uint8_t val) { (void)type; (void)reg; (void)val; return false; }
/* the S9's return to TWRP through its PMU: nothing here (the Pixel restarts by its own watchdog or Power) */
void starlte_return_to_recovery(void);
void starlte_return_to_recovery(void) { printf("FRANKEL: no return-to-recovery path on this board\n"); }

/* SWEET's Qualcomm SPMI (SweetBattery.cpp): not on this board, so SweetBattery keeps its static fallback */
int sweet_spmi_init(void);
int sweet_spmi_init(void) { return ENODEV; }
int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out);
int sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out) { (void)sid; (void)addr; (void)out; return ENODEV; }

/* The loader's core taken over by the kernel (xnu-8796's core0=1, used by hakios.ufs.c's loader-served mode, ufs=3): not on
 * this board, where the kernel has the UFS host itself (ufs_native=4) or nothing */
void hakios_loader_core_take(void);
void hakios_loader_core_take(void) { }
int hakios_loader_core_taken(void);
int hakios_loader_core_taken(void) { return 0; }

/*
 * The S9's drivers are compiled into this board's kernel too (the layer is one tree), and a few of them start from a boot
 * argument alone. Each would program Exynos 9810 addresses, which on the Tensor G5 are other blocks' registers. So a FRANKEL
 * kernel refuses to boot with any of those arguments on its line, before the first of them could run (EARLY_BOOT, first):
 * a stop with this message is safe, a write to an unknown register is not.
 */
#include <kern/startup.h>
static const char *const frankel_s9_args[] = {
	"usb_gadget", "usb_muic", "cp", "wifi_pcie", "brcmfmac", "cam_s9", "cam_rear", "cam_af", "shub", "panel",
	"panel_brightness", "panel_sleep", "panel_gamma", "dpu_probe", "fb_psr", "tmu", "cpufreq_gov", "sweetreboot", "sweetwdog",
	"scionup", "sdroot", "sdprobe", "ufsprobe", "ufsrw", "abox", "max98512", "bt", "sbm_prof", "sweetfb_gpu", "starsmpprobe",
};
static void
frankel_refuse_s9_args(void)
{
	char value[16];

	for (unsigned i = 0; i < sizeof(frankel_s9_args) / sizeof(frankel_s9_args[0]); i++) {
		if (PE_parse_boot_argn(frankel_s9_args[i], value, sizeof(value))) {
			panic("FRANKEL: boot argument \"%s\" starts a Galaxy S9 driver (Exynos 9810 registers): refused on the Pixel 10",
			    frankel_s9_args[i]);
		}
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, frankel_refuse_s9_args);
