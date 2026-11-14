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

/* partitions of the S9's UFS (hakios.shannon.c): none here */
int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes);
int
hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes)
{
	(void)name; (void)offset; (void)dst; (void)len;
	if (bytes) {
		*bytes = 0;
	}
	return ENODEV;
}
int sweet_ufs_rootdev(dev_t *dev);
int sweet_ufs_rootdev(dev_t *dev) { (void)dev; return ENODEV; }

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
