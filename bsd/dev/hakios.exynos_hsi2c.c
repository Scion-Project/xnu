/*
 * Exynos HSI2C: the I2C master of a USI block. sys/hakios.exynos_hsi2c.h says how a driver describes its bus.
 *
 * UEFI sets up none of this, so before the first transfer: force the block's clock gates on and enable its Q-channel
 * (in its CMU), make the USI block an I2C (mode word 4), set its pins to their function, reset the controller, clear
 * USI_CON (its reset value holds the block in reset), then set the timing for the bus speed, master and auto mode.
 *
 * A message: FIFO triggers, master with the channel on, the target's address, the length (read, STOP) in AUTO_CONF, RUN.
 * Done when INT_STAT says so and the transmit FIFO is empty, or when all bytes are in. Abort, no ack, no device and
 * time-out (INT_STAT bits 8-11) reset the bus.
 *
 * Sources: i2c-exynos5.c and soc/samsung/usi_v2.c of Samsung's GPL kernel and the 9810's CMU tables (read for these facts,
 * not copied); the WoA port for the S9+ (github.com/ntdevlabs/exynos9810-woa).
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.xnu.h>


#define CTL                     0x00
#define  CTL_MASTER             (1u << 3)
#define  CTL_RXCHON             (1u << 6)
#define  CTL_TXCHON             (1u << 7)
#define  CTL_SW_RST             (1u << 31)
#define FIFO_CTL                0x04
#define TRAILING                0x08
#define INT_EN                  0x20
#define INT_STAT                0x24
#define  INT_DONE               (1u << 7)
#define  INT_ERRORS             (0xfu << 8)
#define FIFO_STAT               0x30
#define  TX_FULL                (1u << 7)
#define  TX_EMPTY               (1u << 8)
#define  RX_EMPTY               (1u << 24)
#define TX_DATA                 0x34
#define RX_DATA                 0x38
#define CONF                    0x40
#define  CONF_AUTO              (1u << 31)
#define AUTO_CONF               0x44
#define  AUTO_READ              (1u << 16)
#define  AUTO_STOP              (1u << 17)
#define  AUTO_RUN               (1u << 31)
#define TIMEOUT                 0x48
#define TRANS_STATUS            0x50
#define  MASTER_BUSY            (1u << 17)
#define TIMING_FS1              0x60
#define TIMING_FS2              0x64
#define TIMING_FS3              0x68
#define ADDR                    0x70
#define USI_CON                 0xC4

#define CMU_GATE_MANUAL         (1u << 20)
#define CMU_GATE_ON             (1u << 21)
#define USI_MODE_I2C            0x4

/* a moment `us` microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* a register of its own page, wherever in it */
static vm_offset_t
map_word(uint64_t pa)
{
	vm_offset_t page = ml_io_map(pa & ~0xfffULL, 0x1000);

	return page ? page + (pa & 0xfff) : 0;
}

bool
hakios_hsi2c_map(struct hakios_hsi2c *bus, int (*say)(const char *, ...))
{
	bus->regs = ml_io_map(bus->regs_pa, 0x1000);
	bus->usi = map_word(bus->usi_mode_pa);
	bus->cmu = bus->cmu_pa ? ml_io_map(bus->cmu_pa, bus->cmu_size) : 0;
	bus->pins = bus->pins_pa ? map_word(bus->pins_pa) : 0;
	if (!bus->regs || !bus->usi || (bus->cmu_pa && !bus->cmu) || (bus->pins_pa && !bus->pins)) {
		say("%s: a block of the bus could not be mapped\n", bus->name);
		return false;
	}
	if (bus->cmu) {
		uint32_t div = rd(bus->cmu, bus->div);

		say("%s: USI mode %x (4 is I2C), CMU mux %x, divider %x (400 MHz / %u = %u MHz if the mux takes the 400 MHz "
		    "branch; assumed %u), gates %x %x %x %x, Q-channel %x\n", bus->name, *(volatile uint32_t *)bus->usi,
		    rd(bus->cmu, bus->mux), div, (div & 0xf) + 1, 400 / ((div & 0xf) + 1), bus->ipclk_mhz,
		    bus->ngates > 0 ? rd(bus->cmu, bus->gates[0]) : 0, bus->ngates > 1 ? rd(bus->cmu, bus->gates[1]) : 0,
		    bus->ngates > 2 ? rd(bus->cmu, bus->gates[2]) : 0, bus->ngates > 3 ? rd(bus->cmu, bus->gates[3]) : 0,
		    bus->qch ? rd(bus->cmu, bus->qch) : 0);
	}
	if (bus->pins) {
		say("%s: the pins' CON %x (function 2 is I2C)\n", bus->name, *(volatile uint32_t *)bus->pins);
	}
	return true;
}

void
hakios_hsi2c_power(struct hakios_hsi2c *bus)
{
	for (uint32_t i = 0; bus->cmu && i < bus->ngates; i++) {
		wr(bus->cmu, bus->gates[i], rd(bus->cmu, bus->gates[i]) | CMU_GATE_MANUAL | CMU_GATE_ON);
	}
	if (bus->cmu && bus->qch) {
		wr(bus->cmu, bus->qch, rd(bus->cmu, bus->qch) | 3);
	}
	*(volatile uint32_t *)bus->usi = USI_MODE_I2C;
	if (bus->pins) {
		uint32_t con = *(volatile uint32_t *)bus->pins;

		if ((con & bus->pins_mask) != bus->pins_want) {
			*(volatile uint32_t *)bus->pins = (con & ~bus->pins_mask) | bus->pins_want;
		}
		/* no pull on the two pins (PUD at +8, the same 4-bit fields as CON): the device tree's pin-pud = <0> */
		*(volatile uint32_t *)(bus->pins + 8) &= ~bus->pins_mask;
	}
}

/*
 * The bus speed from the input clock, the way both other drivers work it out: the divider for 15 clocks of the input
 * per bit, then the high time and the start's hold as masks of n bits (n at most 7).
 */
static void
bus_timing(struct hakios_hsi2c *bus)
{
	uint32_t div = bus->ipclk_mhz * 1000000 / (bus->bus_hz * 15);
	uint32_t n = (9 * bus->ipclk_mhz) / ((div + 1) * 10);

	if (n > 7) {
		n = 7;
	}
	uint32_t scl_h = (0xFFFFFFFFu >> n << n) & 0xff;
	uint32_t m = n ? n - 1 : 0;
	uint32_t start_hd = (0xFFFFFFFFu >> m << m) & 0xff;

	wr(bus->regs, TIMING_FS3, (rd(bus->regs, TIMING_FS3) & ~(0xffu << 16)) | (div & 0xff) << 16);
	wr(bus->regs, TIMING_FS2, (rd(bus->regs, TIMING_FS2) & ~0xffu) | scl_h);
	wr(bus->regs, TIMING_FS1, (rd(bus->regs, TIMING_FS1) & ~(0xffu << 16)) | start_hd << 16);
}

void
hakios_hsi2c_reset(struct hakios_hsi2c *bus)
{
	vm_offset_t r = bus->regs;

	wr(r, USI_CON, 0);
	wr(r, INT_STAT, rd(r, INT_STAT));
	wr(r, CTL, rd(r, CTL) | CTL_SW_RST);
	delay(10);
	wr(r, CTL, rd(r, CTL) & ~CTL_SW_RST);
	bus_timing(bus);
	wr(r, CTL, CTL_MASTER);
	wr(r, TRAILING, 0xffffff);
	wr(r, TIMEOUT, rd(r, TIMEOUT) & ~(1u << 31));
	wr(r, CONF, rd(r, CONF) | CONF_AUTO);
}

uint32_t
hakios_hsi2c_message(struct hakios_hsi2c *bus, uint8_t addr, bool reading, uint8_t *buf, uint32_t len, bool stop)
{
	vm_offset_t r = bus->regs;
	uint32_t trigger = len < 8 ? len : 8, done = 0, status = 0;
	uint64_t deadline;
	bool late = false;

	bus->transfers++;
	wr(r, INT_STAT, rd(r, INT_STAT));
	wr(r, FIFO_CTL, 0x3 | trigger << 4 | trigger << 16);
	wr(r, CTL, CTL_MASTER | (reading ? CTL_RXCHON : CTL_TXCHON));
	wr(r, ADDR, (uint32_t)addr << 10);
	wr(r, INT_EN, INT_DONE | INT_ERRORS);
	wr(r, AUTO_CONF, len | (reading ? AUTO_READ : 0) | (stop ? AUTO_STOP : 0));
	wr(r, AUTO_CONF, rd(r, AUTO_CONF) | AUTO_RUN);

	/* a second at most, by the clock */
	for (deadline = deadline_us(1000000);;) {
		uint32_t fifo = rd(r, FIFO_STAT);

		status = rd(r, INT_STAT);
		if (status & INT_ERRORS) {
			break;
		}
		if (reading) {
			while (done < len && !(fifo & RX_EMPTY)) {
				buf[done++] = (uint8_t)rd(r, RX_DATA);
				fifo = rd(r, FIFO_STAT);
			}
			if (done == len) {
				break;
			}
		} else {
			while (done < len && !(fifo & TX_FULL)) {
				wr(r, TX_DATA, buf[done++]);
				fifo = rd(r, FIFO_STAT);
			}
			if (done == len && (status & INT_DONE) && (fifo & TX_EMPTY)) {
				break;
			}
		}
		if (mach_absolute_time() > deadline) {
			late = true;
			break;
		}
	}
	/* after a STOP: the master no longer busy (the target may stretch the clock); without one it holds the bus */
	for (deadline = deadline_us(100000); stop && (rd(r, TRANS_STATUS) & MASTER_BUSY) && mach_absolute_time() < deadline;) {
	}
	if ((status & INT_ERRORS) || late) {
		bus->errors++;
		bus->last_error = (status & INT_ERRORS) ? (status & INT_ERRORS) : 1;
		if (bus->errors <= 3) {
			printf("%s: hsi2c failed (addr %02x %s len %u done %u late %d): CTL %08x FIFO_CTL %08x INT_EN %08x INT_STAT %08x "
			    "FIFO_STAT %08x CONF %08x AUTO_CONF %08x TIMEOUT %08x TRANS_STATUS %08x FS1 %08x FS2 %08x FS3 %08x "
			    "ADDR %08x USI_CON %08x USI_OPT %08x; pins CON %08x DAT %08x\n", bus->name, addr, reading ? "read" : "write",
			    len, done, late, rd(r, CTL), rd(r, FIFO_CTL), rd(r, INT_EN), rd(r, INT_STAT), rd(r, FIFO_STAT), rd(r, CONF),
			    rd(r, AUTO_CONF), rd(r, TIMEOUT), rd(r, TRANS_STATUS), rd(r, TIMING_FS1), rd(r, TIMING_FS2),
			    rd(r, TIMING_FS3), rd(r, ADDR), rd(r, USI_CON), rd(r, 0xC8),
			    bus->pins ? *(volatile uint32_t *)bus->pins : 0, bus->pins ? *(volatile uint32_t *)(bus->pins + 4) : 0);
		}
		hakios_hsi2c_reset(bus);
		return bus->last_error;
	}
	return 0;
}

uint32_t
hakios_hsi2c_read_reg(struct hakios_hsi2c *bus, uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len)
{
	uint32_t error = hakios_hsi2c_message(bus, addr, false, &reg, 1, false);

	return error ? error : hakios_hsi2c_message(bus, addr, true, buf, len, true);
}
