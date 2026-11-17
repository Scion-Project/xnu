/*
 * An Exynos SPI master -- the SPI of a USI block of the Exynos 9810 (Samsung's "samsung,exynos-spi", the s3c64xx
 * family) -- polled, without DMA or interrupts, the chip select held by hand. drivers/soc/exynos/hakios.exynos_spi.c; first
 * used for the Galaxy S9's audio codec (hakios.cs47l93.c, USI01 of PERIC0). A driver describes its bus in a struct
 * hakios_exynos_spi and calls map, power, reset, then select / transfer / deselect for each message.
 */
#ifndef _SYS_HAKIOS_EXYNOS_SPI_H_
#define _SYS_HAKIOS_EXYNOS_SPI_H_

#include <stdbool.h>
#include <stdint.h>
#include <mach/vm_types.h>

struct hakios_exynos_spi {
	const char *name;               /* for the log */
	uint64_t regs_pa;               /* the SPI */
	uint64_t usi_mode_pa;           /* the USI's mode word in its system register block: 2 makes it an SPI */
	uint64_t cmu_pa;                /* the CMU block its clocks are in, and in it: */
	uint32_t cmu_size;
	uint32_t gates[4], ngates;      /*   gates forced on (bit 20 manual, bit 21 on) */
	uint32_t qch;                   /*   its Q-channel (bits 0-1 on), 0: none */
	uint32_t div, mux;              /*   the IP clock's divider and its mux (both for the log) */
	uint32_t div_ratio;             /*   the divider's DIVRATIO [3:0] to set, 0xff: left as it is */
	uint32_t ipclk_mhz;             /*   the divider's input (for the log: SCLK is a quarter of the IP clock) */
	uint64_t pins_pa;               /* the CON register of the bank of the bus's pins, 0: left as they are */
	uint32_t pins_mask, pins_want;  /*   their nibbles in CON (and in PUD, at +8): the functions */
	uint32_t pud_mask, pud_want;    /*   and their pulls */
	uint32_t fifo_bytes;            /* the device tree's samsung,spi-fifosize */
	uint32_t mode;                  /* bit 0 CPHA, bit 1 CPOL (Linux's SPI_MODE_n) */
	uint32_t fb_delay;              /* samsung,spi-feedback-delay (FB_CLK [1:0]) */
	/* the bus's own */
	vm_offset_t regs, usi, cmu, pins;
	uint64_t transfers, errors;
	uint32_t last_status;
};

/* Map everything and say what it is set to; writes nothing. false: a block could not be mapped. */
bool hakios_exynos_spi_map(struct hakios_exynos_spi *bus, int (*say)(const char *, ...));
/* The clocks on (and the divider set when div_ratio says), the USI to SPI, the pins to the SPI (the first writes). */
void hakios_exynos_spi_power(struct hakios_exynos_spi *bus);
/* The USI out of its reset, the controller reset: master, no DMA, no interrupt, 8-bit words, the chip select off. */
void hakios_exynos_spi_reset(struct hakios_exynos_spi *bus);
/* The chip select on (low) or off: it stays as set across transfers, so one message may be several transfers. */
void hakios_exynos_spi_select(struct hakios_exynos_spi *bus, bool on);
/*
 * len bytes clocked out and in at once (tx NULL: zeroes sent; rx NULL: what comes back dropped), a FIFO's worth at a
 * time. 0, or the controller's STATUS when it did not finish in time (and the FIFOs are then flushed).
 */
uint32_t hakios_exynos_spi_transfer(struct hakios_exynos_spi *bus, const uint8_t *tx, uint8_t *rx, uint32_t len);
/* The controller's registers, for a driver's own report. */
void hakios_exynos_spi_report(struct hakios_exynos_spi *bus, int (*say)(const char *, ...));

#endif /* _SYS_HAKIOS_EXYNOS_SPI_H_ */
