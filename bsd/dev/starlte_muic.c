/*
 * iOS 27 on STARLTE (8 Nov 2026): close the Galaxy S9's USB-C switch
 * (MAX77705 MUIC CONTROL1 = COM_USB, D+/D- to the AP's USB) so the DWC3 gadget console
 * (hakios.dwc3.c, usb_gadget=5) can be seen by the host. The facts and the opcode sequence are
 * those of Scion's hakios.max77705.c (max77705_usbsw=1); bus through hakios.exynos_hsi2c.c.
 * No interrupt, no VBUS tracking: close it at boot and re-check every 2 s for 2 minutes.
 * Boot-arg usb_muic=1 (default 1 when usb_gadget is set).
 */
#include <stdbool.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/startup.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.xnu.h>

#define MUIC                    0x25
#define OPCODE_WRITE            0x21
#define OPCODE_WRITE_END        0x41
#define OPCODE_READ             0x51
#define OPCODE_CTRL1_R          0x05
#define OPCODE_CTRL1_W          0x06
#define COM_USB                 0x09
#define MUIC_BC_STATUS          0x08

extern int hakios_say(const char *format, ...);

static struct hakios_hsi2c bus = {
	.name = "muic",
	.regs_pa = 0x14360000UL, .usi_mode_pa = 0x14212030UL,
	.cmu_pa = 0x14200000UL, .cmu_size = 0x4000,
	.gates = { 0x2014, 0x20A8, 0x20AC, 0x2074 }, .ngates = 4,
	.qch = 0x3050, .div = 0x1814, .mux = 0x1018,
	.pins_pa = 0, .ipclk_mhz = 400, .bus_hz = 400000,
};
static thread_call_t muic_call;
static int muic_ticks;
static bool muic_up;

static uint32_t
ctrl1_write(uint8_t value)
{
	uint8_t w1[3] = { OPCODE_WRITE, OPCODE_CTRL1_W, value }, w2[2] = { OPCODE_WRITE_END, 0 };
	uint32_t e = hakios_hsi2c_message(&bus, MUIC, false, w1, 3, true);
	return e ? e : hakios_hsi2c_message(&bus, MUIC, false, w2, 2, true);
}

static int
ctrl1_read(void)
{
	uint8_t w1[2] = { OPCODE_WRITE, OPCODE_CTRL1_R }, w2[2] = { OPCODE_WRITE_END, 0 }, a[2] = { 0, 0 };
	uint32_t e = hakios_hsi2c_message(&bus, MUIC, false, w1, 2, true);
	if (e == 0) {
		e = hakios_hsi2c_message(&bus, MUIC, false, w2, 2, true);
	}
	for (int i = 0; e == 0 && i < 10; i++) {
		delay(10000);
		if ((e = hakios_hsi2c_read_reg(&bus, MUIC, OPCODE_READ, a, 2)) == 0 && a[0] == OPCODE_CTRL1_R) {
			return a[1];
		}
	}
	return -1;
}

static void
muic_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	if (!muic_up) {
		if (!hakios_hsi2c_map(&bus, hakios_say)) {
			printf("STARLTE MUIC: HSI2C map failed\n");
			return;
		}
		hakios_hsi2c_power(&bus);
		vm_offset_t g = ml_io_map(0x14220000, 0x1000);         /* GPM14-0 SCL, GPM15-0 SDA: function 2, no pull */
		if (g != 0) {
			for (uint32_t b = 0x180; b <= 0x1A0; b += 0x20) {
				volatile uint32_t *con = (volatile uint32_t *)(g + b), *pud = (volatile uint32_t *)(g + b + 8);
				*con = (*con & ~0xfu) | 2;
				*pud = *pud & ~0xfu;
			}
		}
		hakios_hsi2c_reset(&bus);
		muic_up = true;
	}
	uint8_t bc = 0;
	(void)hakios_hsi2c_read_reg(&bus, MUIC, MUIC_BC_STATUS, &bc, 1);
	int c = ctrl1_read();
	if (c != COM_USB) {
		uint32_t e = ctrl1_write(COM_USB);
		delay(50000);
		int c2 = ctrl1_read();
		printf("STARLTE MUIC: CONTROL1 %#x -> write COM_USB (%u) -> %#x; BC_STATUS %#x (VBUS %u type %u)\n",
		    c, e, c2, bc, bc >> 7, bc & 3);
	} else if (muic_ticks % 15 == 0) {
		printf("STARLTE MUIC: CONTROL1 %#x (USB to the AP); BC_STATUS %#x\n", c, bc);
	}
	if (++muic_ticks < 60) {
		uint64_t deadline;
		clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(muic_call, deadline);
	}
}

__startup_func
static void
starlte_muic_init(void)
{
	uint32_t gadget = 0, on = 1;
	if (!PE_parse_boot_argn("usb_gadget", &gadget, sizeof(gadget)) || gadget == 0) {
		return;
	}
	(void)PE_parse_boot_argn("usb_muic", &on, sizeof(on));
	if (!on) {
		return;
	}
	muic_call = thread_call_allocate_with_priority(muic_tick, NULL, THREAD_CALL_PRIORITY_KERNEL_HIGH);
	thread_call_enter(muic_call);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, starlte_muic_init);
