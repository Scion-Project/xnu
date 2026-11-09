/*
 * An Exynos SPI master (a USI block in SPI mode) as this port drives it: drivers/soc/exynos/hakios.exynos_spi.c.
 * A driver describes its bus (where the controller is, the USI mode word, the clock gates to force on, the pins)
 * and calls hakios_exynos_spi_map / _power / _reset / _select / _transfer; one driver per bus, no locking.
 */
#ifndef _SYS_HAKIOS_EXYNOS_SPI_H_
#define _SYS_HAKIOS_EXYNOS_SPI_H_

#include <stdbool.h>
#include <stdint.h>
#include <mach/vm_types.h>

struct hakios_exynos_spi {
	const char *name;               /* for the log */
	uint64_t regs_pa;               /* the SPI block (USI SPI mode registers) */
	uint64_t usi_mode_pa;           /* the USI's mode word in its system register block: 2 makes it an SPI */
	uint64_t cmu_pa;                /* the CMU block its clocks are in, and in it: */
	uint32_t cmu_size;
	uint32_t gates[4], ngates;      /*   gates forced on (bit 20 manual, bit 21 on) */
	uint32_t qch;                   /*   its Q-channel (bits 0-1 on), 0: none */
	uint32_t div, mux;              /*   the IP clock's divider and mux */
	uint32_t div_ratio;             /*   the divider value to write (SCLK = IP_clock / (div_ratio+1) / 4) */
	uint32_t ipclk_mhz;             /* the SPI's input clock (for the log) */
	uint64_t pins_pa;               /* the CON register of the GPIO bank for the SPI pins, 0: left as they are */
	uint32_t pins_mask, pins_want;  /*   ... and their nibbles: pin function for SPI */
	uint32_t pud_mask, pud_want;    /* the PUD register mask and desired values (nSS pull-up etc.) */
	uint32_t fifo_bytes;            /* FIFO depth in bytes (must be a power of two, ≤ 256) */
	uint32_t mode;                  /* SPI mode: bit 1 CPOL, bit 0 CPHA */
	uint32_t fb_delay;              /* FB_CLK feedback delay (0–3) */
	/* the bus's own (set by hakios_exynos_spi_map) */
	vm_offset_t regs, usi, cmu, pins;
	uint64_t transfers, errors;
	uint32_t last_status;
};

/* Map everything and say what it is set to; writes nothing. false: a block could not be mapped. */
bool hakios_exynos_spi_map(struct hakios_exynos_spi *bus, int (*say)(const char *, ...));
/* The clocks on, the USI to SPI mode, the pins to their SPI function (the first writes). */
void hakios_exynos_spi_power(struct hakios_exynos_spi *bus);
/* The controller reset and configured for bus->mode and bus->fb_delay. */
void hakios_exynos_spi_reset(struct hakios_exynos_spi *bus);
/* Assert (on=true) or deassert (on=false) nSS (chip select). */
void hakios_exynos_spi_select(struct hakios_exynos_spi *bus, bool on);
/*
 * Transfer len bytes: send from tx (NULL: send zeros) and receive into rx (NULL: discard). Returns 0 on success or
 * the STATUS register value that caused the error; a failed transfer leaves the FIFO flushed and the driver can retry.
 */
uint32_t hakios_exynos_spi_transfer(struct hakios_exynos_spi *bus, const uint8_t *tx, uint8_t *rx, uint32_t len);
/* Log the controller's current register values. */
void hakios_exynos_spi_report(struct hakios_exynos_spi *bus, int (*say)(const char *, ...));

#endif /* _SYS_HAKIOS_EXYNOS_SPI_H_ */
