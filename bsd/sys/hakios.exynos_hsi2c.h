/*
 * An Exynos HSI2C (an I2C master of a USI block) as this port drives it: drivers/soc/exynos/hakios.exynos_hsi2c.c.
 * A driver describes its bus (where the controller is, the USI mode word that makes the block an I2C, the clock gates
 * to force on, the pins) and calls these from its own thread; one driver per bus, no locking.
 */
#ifndef _SYS_HAKIOS_EXYNOS_HSI2C_H_
#define _SYS_HAKIOS_EXYNOS_HSI2C_H_

#include <stdbool.h>
#include <stdint.h>
#include <mach/vm_types.h>

struct hakios_hsi2c {
	const char *name;               /* for the log */
	uint64_t regs_pa;               /* the HSI2C */
	uint64_t usi_mode_pa;           /* the USI's mode word in its system register block: 4 makes it an I2C */
	uint64_t cmu_pa;                /* the CMU block its clocks are in, and in it: */
	uint32_t cmu_size;
	uint32_t gates[4], ngates;      /*   gates forced on (bit 20 manual, bit 21 on) */
	uint32_t qch;                   /*   its Q-channel (bits 0-1 on), 0: none */
	uint32_t div, mux;              /*   the IP clock's divider and mux, for the log */
	uint64_t pins_pa;               /* the CON register of the bank of SCL and SDA, 0: left as they are */
	uint32_t pins_mask, pins_want;  /*   ... and their two nibbles: function 2, I2C */
	uint32_t ipclk_mhz;             /* the HSI2C's input clock (both other drivers assume 200) */
	uint32_t bus_hz;
	/* the bus's own */
	vm_offset_t regs, usi, cmu, pins;
	uint64_t transfers, errors;
	uint32_t last_error;
};

/* Map everything and say what it is set to; writes nothing. false: a block could not be mapped. */
bool hakios_hsi2c_map(struct hakios_hsi2c *bus, int (*say)(const char *, ...));
/* The clocks on, the USI to I2C, the pins to I2C (the first writes). */
void hakios_hsi2c_power(struct hakios_hsi2c *bus);
/* The controller reset and set to bus_hz. */
void hakios_hsi2c_reset(struct hakios_hsi2c *bus);
/*
 * One message to or from addr (7 bits), with a STOP after it or not (a repeated start follows then). 0, the INT_STAT
 * error bits (abort, no ack, no device, time-out: 0xf00), or 1 for a time-out of ours; a failed one leaves the bus
 * reset.
 */
uint32_t hakios_hsi2c_message(struct hakios_hsi2c *bus, uint8_t addr, bool reading, uint8_t *buf, uint32_t len,
    bool stop);
/* A register read the SMBus way: reg written, a repeated start, len bytes read. */
uint32_t hakios_hsi2c_read_reg(struct hakios_hsi2c *bus, uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len);

#endif /* _SYS_HAKIOS_EXYNOS_HSI2C_H_ */
