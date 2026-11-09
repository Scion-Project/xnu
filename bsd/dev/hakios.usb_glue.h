/*
 * What the USB gadget driver (drivers/usb/hakios.dwc3.c, device-neutral: a DesignWare DWC3 in device mode with a
 * CDC-ACM function) needs from the SoC it runs on: where the controller is, and how its clocks, power and PHY are
 * brought up. One of these per SoC; the Exynos 9810's is drivers/usb/hakios.exynos_usb.c.
 */
#ifndef _HAKIOS_USB_GLUE_H_
#define _HAKIOS_USB_GLUE_H_

#include <stdbool.h>
#include <stdint.h>

#define USB_GLUE_CLK    0x1u    /* usb_gadget_clk=1: open the controller's clocks where the boot left them shut */
#define USB_GLUE_LDO    0x2u    /* usb_gadget_ldo=1: the PHY's supplies into Android's USB mode */

typedef int (*usb_glue_say_t)(const char *format, ...);

struct hakios_usb_glue {
	const char      *name;
	uint64_t        dwc3_pa;        /* the DWC3's registers (its global ones at +0xc100) */
	uint32_t        dwc3_size;
	/* step 1: read what the boot left and say it; true when the DWC3's registers can be read without a bus hang */
	bool            (*look)(usb_glue_say_t say, uint32_t flags);
	/* step 2: power, clocks, the PHY up in device mode (high speed; the SuperSpeed lanes held idle); 0 or an errno */
	int             (*phy_on)(usb_glue_say_t say, uint32_t flags);
	/* a line of the PHY's state (the D+/D- line state, VBUS as the link sees it), for the log while enumerating */
	void            (*phy_state)(char *out, unsigned size);
	/* the way back to the loader: the PHY into reset and isolated again (the link already stopped) */
	void            (*phy_off)(void);
};

/* drivers/usb/hakios.exynos_usb.c; NULL from hakios.stubs.c when that driver is left out */
const struct hakios_usb_glue *hakios_usb_glue_exynos9810(void);

#endif /* _HAKIOS_USB_GLUE_H_ */
