/*
 * iOS 27 on STARLTE (8 Nov 2026): what Scion/hakios's DWC3 gadget console
 * (hakios.dwc3.c + hakios.exynos_usb.c, usb_gadget=5) needs from its own kernel, provided on Starbite's
 * xnu-12377 STARLTE tree: the log tap (fed from a5xelte_log_append, the console ring every printf goes
 * through) and stubs for the hakios subsystems this tree does not have.
 */
#include <stdint.h>
#include <stdbool.h>
#include <os/atomic_private.h>
#include <arm64/proc_reg.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/systm.h>

extern void a5xelte_log_snapshot(const char **ring, uint32_t *size, uint64_t *total);

static void (*volatile starlte_usb_tap)(char c);
static uint32_t starlte_usb_tap_lock;

static bool
tap_lock(uint64_t *daif)
{
	bool locked = false;
	*daif = __builtin_arm_rsr64("DAIF");
	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	for (uint32_t tries = 0; !locked && tries < 1000000; tries++) {
		locked = os_atomic_cmpxchg(&starlte_usb_tap_lock, 0, 1, acquire);
	}
	return locked;
}

static void
tap_unlock(bool locked, uint64_t daif)
{
	if (locked) {
		os_atomic_store(&starlte_usb_tap_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
}

/* Called by a5xelte_log_append (bsd/dev/a5xelte_emmc.c) with every piece of console text. */
void starlte_usb_tap_text(const char *c, int size);
void
starlte_usb_tap_text(const char *c, int size)
{
	void (*tap)(char) = starlte_usb_tap;
	uint64_t daif;
	if (tap == NULL || size <= 0) {
		return;
	}
	bool locked = tap_lock(&daif);
	for (int i = 0; i < size; i++) {
		if (c[i] != '\r') {
			tap(c[i]);
		}
	}
	tap_unlock(locked, daif);
}

uint32_t loader_log_tap_set(void (*tap)(char c), void (*history)(const char *text, uint32_t len));
uint32_t
loader_log_tap_set(void (*tap)(char c), void (*history)(const char *text, uint32_t len))
{
	const char *ring;
	uint32_t rsize;
	uint64_t total, daif;
	bool locked = tap_lock(&daif);
	a5xelte_log_snapshot(&ring, &rsize, &total);
	if (history != NULL && total > 0) {
		if (total <= rsize) {
			history(ring, (uint32_t)total);
		} else {
			uint32_t at = (uint32_t)(total % rsize);
			history(ring + at, rsize - at);
			history(ring, at);
		}
	}
	starlte_usb_tap = tap;
	tap_unlock(locked, daif);
	return (uint32_t)total;
}

void loader_log_line(const char *line);
void
loader_log_line(const char *line)
{
	printf("%s", line);
}

void loader_return(void);
extern void starlte_return_to_recovery(void);
void loader_return(void) { starlte_return_to_recovery(); }
/* hakios_wifi_stop/_report, hakios_sta_console_scan, hakios_net_console_*: the Wi-Fi drivers' own (bsd/dev/starlte_wifi.c) */
void hakios_var_save_on_return(void);
void hakios_var_save_on_return(void) {}
void hakios_ufs_summary(const char *when, int how);
void hakios_ufs_summary(__unused const char *when, __unused int how) {}
void hakios_adreno_stop(void);
void hakios_adreno_stop(void) {}
#define STUB_ARG(fn) void fn(const char *arg); void fn(__unused const char *arg) { printf("usb: " #fn ": not on this kernel\n"); }
STUB_ARG(hakios_bt_console)
STUB_ARG(hakios_input_console_touch)
STUB_ARG(hakios_input_console_button)
STUB_ARG(hakios_abox_console)
STUB_ARG(hakios_cs47l93_console)
STUB_ARG(hakios_ssp_console)
STUB_ARG(hakios_max77705_usb_console)
void hakios_adsp_stop(void);
void hakios_adsp_stop(void) {}
void hakios_max77705_usb_close(void);
void hakios_max77705_usb_close(void) {}
int hakios_panel_brightness(int level);
/* the real one is bsd/dev/hakios.s6e3ha8.c now */
/* hakios_exynos_cpufreq_touch(): the CPU governor's, bsd/dev/hakios.exynos_cpufreq.c (a no-op without cpufreq_gov=1) */
int hakios_fb_scanned(uintptr_t *base, uint32_t *width, uint32_t *height, uint32_t *rowbytes, unsigned *index);
int hakios_fb_scanned(__unused uintptr_t *base, __unused uint32_t *width, __unused uint32_t *height,
    __unused uint32_t *rowbytes, __unused unsigned *index) { return ENODEV; }
bool hakios_acpm_init(int (*say)(const char *, ...));
bool hakios_acpm_init(__unused int (*say)(const char *, ...)) { return false; }
int hakios_acpm_pmic_read(uint32_t type, uint32_t reg, uint8_t *value);
int hakios_acpm_pmic_read(__unused uint32_t type, __unused uint32_t reg, __unused uint8_t *value) { return ENODEV; }
int hakios_acpm_pmic_update(uint32_t type, uint32_t reg, uint8_t value, uint8_t mask);
int hakios_acpm_pmic_update(__unused uint32_t type, __unused uint32_t reg, __unused uint8_t value, __unused uint8_t mask) { return ENODEV; }

#include <stdarg.h>
int hakios_say(const char *format, ...) __printflike(1, 2);
int
hakios_say(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	return 0;
}

/* the console's camshot/irshot/swipe and the kern.hakios sysctl node, which this tree does not have */
#include <sys/sysctl.h>
SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW | CTLFLAG_LOCKED, NULL, "hakios (USB console only)");
void hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void hakios_touch_post(__unused uint32_t type, __unused uint32_t finger, __unused uint32_t x, __unused uint32_t y) {}
const char *hakios_exynos_camera_shot(uint32_t n, int pattern);
const char *hakios_exynos_camera_shot(__unused uint32_t n, __unused int pattern) { return "no camera driver on this kernel"; }
const char *hakios_exynos_iris_shot(uint32_t n);
const char *hakios_exynos_iris_shot(__unused uint32_t n) { return "no iris driver on this kernel"; }
