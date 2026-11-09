/*
 * Early Exynos7580 ZT7538 bring-up probe.  The working native Linux
 * port uses I2C2 at 0x13850000, GPC3-0/1, GPA1-1 and S2MPU03 LDO33.
 * Opt-in touch logging enables only the proven S2MPU03 LDO33 supply and
 * observes ZT7538 contacts before userland. It never programs touch NVM.
 */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void thread_terminate_self(void);
extern void sweet_hid_touch(int finger, int down, int x, int y);
void a5xelte_touch_probe(void);
static bool a5touch_controller_init(void);
static bool a5touch_report_packet(const uint8_t packet[34], unsigned *lines);
static void a5touch_release_contacts(void);

static vm_offset_t a5_touch_bus;
static vm_offset_t a5_touch_alive;
static vm_offset_t a5_touch_hsi;
static int a5_touch_sleep;
static bool a5_touch_active[5];
static uint16_t a5_touch_x[5], a5_touch_y[5];

static uint16_t
a5touch_le16(const uint8_t *bytes)
{
	return (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
}

static uint32_t
a5touch_read(vm_offset_t base, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
a5touch_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* Exynos7580 MIF HSI2C3 AUTO mode, matching Linux's 400 kHz timing. */
static void
a5pmic_bus_init(vm_offset_t hsi)
{
	a5touch_write(hsi, 0x00, 0x80000009u);
	a5touch_write(hsi, 0x00, 0x00000009u);
	a5touch_write(hsi, 0x48, 0); /* Linux disables the hardware timeout. */
	a5touch_write(hsi, 0x60, 0x78787800u);
	a5touch_write(hsi, 0x64, 0x3c007878u);
	a5touch_write(hsi, 0x68, 0x000000f0u);
	a5touch_write(hsi, 0x6c, 0x3cu);
	a5touch_write(hsi, 0x08, 0xfu);
	a5touch_write(hsi, 0x40, 0x80000000u);
	a5touch_write(hsi, 0x20, 0); /* Poll without an unregistered XNU IRQ. */
}

static bool
a5pmic_message(vm_offset_t hsi, bool receive, uint8_t *bytes,
    unsigned count, bool stop)
{
	uint32_t ctl;
	uint32_t cfg;
	uint32_t last_irq = 0;
	bool transfer_done = false;
	unsigned done = 0;
	if (!count || count > 4) {
		return false;
	}
	/* Exynos7580 is EXYNOS7: TRANS_STATUS[3:0] is an FSM state,
	 * not the Exynos5 completion/error bitmap. Linux uses INT_STATUS. */
	a5touch_write(hsi, 0x24, a5touch_read(hsi, 0x24));
	a5touch_write(hsi, 0x04, 3u | (count << (receive ? 4 : 16)));
	ctl = 9u | (receive ? 0x40u : 0x80u);
	a5touch_write(hsi, 0x70, 0x66u << 10);
	a5touch_write(hsi, 0x00, ctl);
	cfg = count | (receive ? 0x10000u : 0) | (stop ? 0x20000u : 0);
	/* GIC SPI for this controller is masked in the A5XELTE platform;
	 * enable the controller's status bits and service them by polling. */
	a5touch_write(hsi, 0x20, 0xf80u | (receive ? 0x42u : 1u));
	a5touch_write(hsi, 0x44, cfg | 0x80000000u);
	for (unsigned us = 0; us < 100000; us++) {
		uint32_t fifo = a5touch_read(hsi, 0x30);
		uint32_t irq = a5touch_read(hsi, 0x24);
		if (irq) {
			last_irq |= irq;
			a5touch_write(hsi, 0x24, irq);
		}
		if (irq & 0xf00u) {
			printf("A5XELTE TOUCH: PMIC HSI2C error rx=%d done=%u/%u irq=%#x state=%#x fifo=%#x\n",
			    receive, done, count, last_irq, a5touch_read(hsi, 0x50), fifo);
			goto fail;
		}
		if (irq & 0x80u) {
			transfer_done = true;
		}
		if (receive) {
			while (done < count && ((a5touch_read(hsi, 0x30) >> 16) & 0x7fu)) {
				bytes[done++] = (uint8_t)a5touch_read(hsi, 0x38);
			}
		} else {
			while (done < count && !(a5touch_read(hsi, 0x30) & (1u << 7))) {
				a5touch_write(hsi, 0x34, bytes[done++]);
			}
			if (done == count) {
				a5touch_write(hsi, 0x20, 0xf80u);
			}
		}
		if (done == count && transfer_done) {
			a5touch_write(hsi, 0x20, 0);
			if (stop) {
				for (unsigned n = 0; n < 100000; n++) {
					if (!(a5touch_read(hsi, 0x50) & (1u << 17))) {
						return true;
					}
					delay(1);
				}
				printf("A5XELTE TOUCH: PMIC STOP busy irq=%#x state=%#x\n",
				    last_irq, a5touch_read(hsi, 0x50));
				goto fail;
			}
			return true;
		}
		delay(1);
	}
	printf("A5XELTE TOUCH: PMIC HSI2C timeout rx=%d done=%u/%u irq=%#x state=%#x fifo=%#x\n",
	    receive, done, count, last_irq | a5touch_read(hsi, 0x24),
	    a5touch_read(hsi, 0x50), a5touch_read(hsi, 0x30));
fail:
	a5touch_write(hsi, 0x20, 0);
	a5pmic_bus_init(hsi);
	return false;
}

static bool
a5pmic_read(vm_offset_t hsi, uint8_t reg, uint8_t *value)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		uint8_t address = reg;
		if (a5pmic_message(hsi, false, &address, 1, false) &&
		    a5pmic_message(hsi, true, value, 1, true)) {
			return true;
		}
		delay(1000);
	}
	return false;
}

static bool
a5pmic_write(vm_offset_t hsi, uint8_t reg, uint8_t value)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		uint8_t data[2] = {reg, value};
		if (a5pmic_message(hsi, false, data, 2, true)) {
			return true;
		}
		delay(1000);
	}
	return false;
}

static bool
a5touch_power_on(vm_offset_t mifpins)
{
	vm_offset_t hsi = ml_io_map(0x104c0000, 0x2000);
	uint32_t old_con;
	uint32_t old_pud;
	uint8_t id = 0xff;
	uint8_t ldo = 0xff;
	uint8_t check = 0xff;
	if (!hsi) {
		printf("A5XELTE TOUCH: PMIC HSI2C mapping failed\n");
		return false;
	}
	old_con = a5touch_read(mifpins, 0);
	old_pud = a5touch_read(mifpins, 8);
	a5touch_write(mifpins, 8, (old_pud & ~0xfu) | 0xfu);
	a5touch_write(mifpins, 0, (old_con & ~0xffu) | 0x22u);
	delay(100);
	a5pmic_bus_init(hsi);
	if (!a5pmic_read(hsi, 0x00, &id) ||
	    !a5pmic_read(hsi, 0x4e, &ldo)) {
		printf("A5XELTE TOUCH: PMIC identity/control read failed\n");
		goto fail;
	}
	printf("A5XELTE TOUCH: S2MPU03 ID=%#x LDO33=%#x\n", id, ldo);
	/* Working Linux device read ID 0x02 and LDO33 0x3c before enable. */
	if (id != 0x02 || (ldo & 0x3fu) != 0x3cu) {
		printf("A5XELTE TOUCH: unexpected PMIC or LDO33 voltage, leave off\n");
		goto fail;
	}
	if (!(ldo & 0xc0u)) {
		if (!a5pmic_write(hsi, 0x4e, ldo | 0xc0u)) {
			printf("A5XELTE TOUCH: LDO33 enable failed\n");
			goto fail;
		}
	}
	if (!a5pmic_read(hsi, 0x4e, &check) || check != (ldo | 0xc0u)) {
		printf("A5XELTE TOUCH: LDO33 readback failed value=%#x\n", check);
		goto fail;
	}
	printf("A5XELTE TOUCH: LDO33 enabled at 3.3 V, readback=%#x\n", check);
	a5_touch_hsi = hsi;
	delay(15000);
	return true;
fail:
	a5touch_write(mifpins, 0, old_con);
	a5touch_write(mifpins, 8, old_pud);
	return false;
}

/* S3C2440 I2C2: one bounded message, with STOP between Zinitix phases. */
static bool
a5touch_wait_irq(vm_offset_t i2c)
{
	for (unsigned n = 0; n < 20000; n++) {
		if (a5touch_read(i2c, 0) & 0x10u) {
			return true;
		}
		delay(1);
	}
	return false;
}

static void
a5touch_clear_irq(vm_offset_t i2c)
{
	a5touch_write(i2c, 0, a5touch_read(i2c, 0) & ~0x10u);
}

static void
a5touch_stop(vm_offset_t i2c)
{
	a5touch_write(i2c, 4, a5touch_read(i2c, 4) & ~0x20u);
	a5touch_clear_irq(i2c);
	for (unsigned n = 0; n < 5000; n++) {
		if (!(a5touch_read(i2c, 4) & 0x20u)) {
			break;
		}
		delay(1);
	}
}

static bool
a5touch_message(vm_offset_t i2c, bool receive, uint8_t *bytes, unsigned count)
{
	const uint32_t mode = receive ? 0x90u : 0xd0u;
	bool ok = false;
	if (!count || count > 34) {
		return false;
	}
	for (unsigned n = 0; n < 5000; n++) {
		if (!(a5touch_read(i2c, 4) & 0x20u)) {
			break;
		}
		if (n == 4999) {
			printf("A5XELTE TOUCH: I2C2 busy before message\n");
			return false;
		}
		delay(1);
	}
	/* Conservative divider; the working Linux controller also sets ACK/IRQ. */
	a5touch_write(i2c, 0, 0xe3u);
	a5touch_write(i2c, 0x10, 7u); /* S3C2440 SDA delay/filter */
	a5touch_write(i2c, 4, mode);
	a5touch_write(i2c, 0xc, 0x40u | (receive ? 1u : 0u));
	delay(1);
	a5touch_write(i2c, 4, mode | 0x20u);
	if (!a5touch_wait_irq(i2c)) {
		printf("A5XELTE TOUCH: I2C2 address timeout stat=%#x con=%#x\n",
		    a5touch_read(i2c, 4), a5touch_read(i2c, 0));
		goto out;
	}
	if (a5touch_read(i2c, 4) & 1u) {
		printf("A5XELTE TOUCH: I2C2 address NACK stat=%#x\n",
		    a5touch_read(i2c, 4));
		goto out;
	}
	for (unsigned n = 0; n < count; n++) {
		if (receive) {
			if (n + 1 == count) {
				a5touch_write(i2c, 0, a5touch_read(i2c, 0) & ~0x80u);
			}
		} else {
			a5touch_write(i2c, 0xc, bytes[n]);
			delay(1);
		}
		a5touch_clear_irq(i2c);
		if (!a5touch_wait_irq(i2c)) {
			printf("A5XELTE TOUCH: I2C2 data timeout byte=%u stat=%#x\n",
			    n, a5touch_read(i2c, 4));
			goto out;
		}
		if (receive) {
			bytes[n] = (uint8_t)a5touch_read(i2c, 0xc);
		} else if (a5touch_read(i2c, 4) & 1u) {
			printf("A5XELTE TOUCH: I2C2 data NACK byte=%u stat=%#x\n",
			    n, a5touch_read(i2c, 4));
			goto out;
		}
	}
	ok = true;
out:
	a5touch_stop(i2c);
	return ok;
}

static void
a5touch_chip_code_trial(vm_offset_t nfc)
{
	vm_offset_t i2c = ml_io_map(0x13850000, 0x1000);
	uint32_t old_con;
	uint32_t old_pud;
	uint8_t reg[2] = {0x00, 0xcc};
	uint8_t code[2] = {0, 0};
	bool sent;
	bool got;
	if (!i2c) {
		printf("A5XELTE TOUCH: I2C2 mapping failed\n");
		return;
	}
	old_con = a5touch_read(nfc, 0);
	old_pud = a5touch_read(nfc, 8);
	/* GPC3-0/1: Linux touch_bus function 2 and PUD 3; preserve other pins. */
	a5touch_write(nfc, 8, (old_pud & ~0xfu) | 0xfu);
	a5touch_write(nfc, 0, (old_con & ~0xffu) | 0x22u);
	delay(100);
	sent = a5touch_message(i2c, false, reg, sizeof(reg));
	if (sent) {
		delay(75); /* ZT7538 requires a STOP and 50 us before receive. */
	}
	got = sent && a5touch_message(i2c, true, code, sizeof(code));
	printf("A5XELTE TOUCH: I2C2 chip-code sent=%d received=%d value=%#x stat=%#x con=%#x\n",
	    sent, got, (unsigned)code[0] | ((unsigned)code[1] << 8),
	    a5touch_read(i2c, 4), a5touch_read(i2c, 0));
	a5touch_write(nfc, 0, old_con);
	a5touch_write(nfc, 8, old_pud);
}

static bool
a5touch_send(uint8_t *data, unsigned length)
{
	for (unsigned attempt = 0; attempt < 8; attempt++) {
		if (a5touch_message(a5_touch_bus, false, data, length)) {
			return true;
		}
		delay(1100);
	}
	return false;
}

static bool
a5touch_cmd(uint16_t reg)
{
	uint8_t data[2] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
	bool ok = a5touch_send(data, 2);
	delay(20);
	return ok;
}

static bool
a5touch_write_reg(uint16_t reg, uint16_t value)
{
	uint8_t data[4] = {(uint8_t)reg, (uint8_t)(reg >> 8),
	    (uint8_t)value, (uint8_t)(value >> 8)};
	bool ok = a5touch_send(data, 4);
	delay(20);
	return ok;
}

static bool
a5touch_read_reg(uint16_t reg, uint8_t *data, unsigned length)
{
	uint8_t address[2] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
	if (!a5touch_send(address, 2)) {
		return false;
	}
	delay(75); /* The ZT7538 requires STOP then at least 50 us. */
	if (!a5touch_message(a5_touch_bus, true, data, length)) {
		return false;
	}
	delay(20);
	return true;
}

static bool
a5touch_read_u16(uint16_t reg, uint16_t *value)
{
	uint8_t data[2] = {0, 0};
	if (!a5touch_read_reg(reg, data, sizeof(data))) {
		return false;
	}
	*value = a5touch_le16(data);
	return true;
}

static void
a5touch_sleep_5ms(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(5, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&a5_touch_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
a5touch_recover(void)
{
	uint8_t ldo = 0;
	uint8_t check = 0;
	a5touch_release_contacts();
	if (!a5_touch_hsi || !a5pmic_read(a5_touch_hsi, 0x4e, &ldo) ||
	    (ldo & 0x3fu) != 0x3cu ||
	    !a5pmic_write(a5_touch_hsi, 0x4e, ldo & ~0xc0u)) {
		printf("A5XELTE TOUCH: LDO33 recovery power-off failed\n");
		return false;
	}
	delay(50000);
	if (!a5pmic_write(a5_touch_hsi, 0x4e, ldo | 0xc0u) ||
	    !a5pmic_read(a5_touch_hsi, 0x4e, &check) || check != (ldo | 0xc0u)) {
		printf("A5XELTE TOUCH: LDO33 recovery power-on failed value=%#x\n", check);
		return false;
	}
	delay(50000);
	return a5touch_controller_init();
}

static void
a5touch_log_thread(void *arg, wait_result_t result)
{
	unsigned lines = 0;
	unsigned errors = 0;
	unsigned idle_ticks = 0;
	unsigned recoveries = 0;
	(void)arg;
	(void)result;
	printf("A5XELTE TOUCH: contact HID worker started (GPA1-1)\n");
	for (;;) {
		/* ZT7538 holds INTN low until CLEAR_INT_STATUS. */
		if (!(a5touch_read(a5_touch_alive, 0x24) & 2u)) {
			uint8_t packet[34] = {0};
			bool read_ok = a5touch_read_reg(0x0080, packet, sizeof(packet));
			bool ack_ok = a5touch_cmd(0x0003);
			idle_ticks = 0;
			if (!read_ok || !ack_ok ||
			    (read_ok && ack_ok && !a5touch_report_packet(packet, &lines))) {
				if (++errors <= 8) {
					printf("A5XELTE TOUCH: packet/ACK failed read=%d ack=%d errors=%u\n",
					    read_ok, ack_ok, errors);
				}
			} else {
				errors = 0;
			}
		} else if (idle_ticks < 600) {
			idle_ticks++;
		}
		if (errors >= 8 || idle_ticks >= 600) {
			printf("A5XELTE TOUCH: recovery reason=%s attempt=%u\n",
			    errors >= 8 ? "transfer" : "health IRQ", recoveries + 1);
			if (!a5touch_recover() && ++recoveries >= 3) {
				printf("A5XELTE TOUCH: worker stopped after recovery failures\n");
				break;
			}
			errors = 0;
			idle_ticks = 0;
		}
		a5touch_sleep_5ms();
	}
	a5touch_release_contacts();
	thread_terminate_self();
}

static void
a5touch_release_contacts(void)
{
	for (unsigned slot = 0; slot < 5; slot++) {
		if (a5_touch_active[slot]) {
			sweet_hid_touch(slot, 0, a5_touch_x[slot], a5_touch_y[slot]);
			a5_touch_active[slot] = false;
		}
	}
}

/* Native Linux mode-0 packet policy, followed by the existing iOS HID
 * digitizer service.  A heartbeat leaves all slots unchanged. */
static bool
a5touch_report_packet(const uint8_t packet[34], unsigned *lines)
{
	uint16_t status = a5touch_le16(packet);
	unsigned count = packet[2];
	if (status == 0) {
		return true;
	}
	if (status == 0xffffu || status == 1u || (status & (1u << 13)) || count > 5) {
		printf("A5XELTE TOUCH: corrupt packet status=%#x count=%u\n", status, count);
		return false;
	}
	for (unsigned slot = 0; slot < 5; slot++) {
		unsigned at = 4 + 6 * slot;
		uint16_t x = a5touch_le16(packet + at);
		uint16_t y = a5touch_le16(packet + at + 2);
		unsigned sub = packet[at + 5];
		bool active = (status & (1u << 11)) && !(status & 0x30u) &&
		    (sub & 1u) && !(sub & 8u) && x <= 1080 && y <= 1920;
		if (active) {
			if (*lines < 256 && (!a5_touch_active[slot] ||
			    a5_touch_x[slot] != x || a5_touch_y[slot] != y)) {
				printf("A5XELTE TOUCH: contact slot=%u x=%u y=%u width=%u flags=%#x status=%#x\n",
				    slot, x, y, packet[at + 4], sub, status);
				(*lines)++;
			}
			a5_touch_x[slot] = x;
			a5_touch_y[slot] = y;
			a5_touch_active[slot] = true;
			sweet_hid_touch(slot, 1, x, y);
		} else if (a5_touch_active[slot]) {
			sweet_hid_touch(slot, 0, a5_touch_x[slot], a5_touch_y[slot]);
			a5_touch_active[slot] = false;
		}
	}
	return true;
}

static bool
a5touch_controller_init(void)
{
	uint16_t chip = 0;
	uint16_t crc = 0;
	uint16_t x = 0;
	uint16_t y = 0;
	/* Native ZT7538 power-on sequence; abort on each failed transfer. */
	if (!a5touch_write_reg(0xc000, 1) ||
	    !a5touch_read_u16(0xcc00, &chip) || chip != 0xe538) {
		printf("A5XELTE TOUCH: chip-code failed value=%#x\n", chip);
		goto fail;
	}
	printf("A5XELTE TOUCH: ZT7538 chip-code=%#x\n", chip);
	if (!a5touch_cmd(0xc004) || !a5touch_write_reg(0xc002, 1)) {
		printf("A5XELTE TOUCH: firmware init sequence failed\n");
		goto fail;
	}
	delay(2000);
	if (!a5touch_write_reg(0xc001, 1)) {
		printf("A5XELTE TOUCH: program start failed\n");
		goto fail;
	}
	delay(150000);
	if (!a5touch_read_u16(0x012c, &crc) || crc != 0x55aau ||
	    !a5touch_read_u16(0x00c0, &x) || !a5touch_read_u16(0x00c1, &y)) {
		printf("A5XELTE TOUCH: firmware CRC/geometry failed crc=%#x x=%u y=%u\n",
		    crc, x, y);
		goto fail;
	}
	printf("A5XELTE TOUCH: firmware CRC=%#x geometry=%ux%u\n", crc, x, y);
	if (!a5touch_cmd(0x0000) || !a5touch_write_reg(0x00f0, 0) ||
	    !a5touch_write_reg(0x0010, 0) ||
	    !a5touch_write_reg(0x00f0, 0x003f)) {
		printf("A5XELTE TOUCH: mode 0 setup failed\n");
		goto fail;
	}
	for (unsigned n = 0; n < 10; n++) {
		if (!a5touch_cmd(0x0003)) {
			printf("A5XELTE TOUCH: interrupt queue ACK failed\n");
			goto fail;
		}
	}
	if (!a5touch_write_reg(0x00f1, 100)) {
		printf("A5XELTE TOUCH: heartbeat setup failed\n");
		goto fail;
	}
	printf("A5XELTE TOUCH: mode 0 ready, INTN=%u\n",
	    (unsigned)!!(a5touch_read(a5_touch_alive, 0x24) & 2u));
	return true;
fail:
	return false;
}

static void
a5touch_start_early(vm_offset_t nfc, vm_offset_t alive)
{
	uint32_t con;
	uint32_t pud;
	thread_t thread;
	a5_touch_bus = ml_io_map(0x13850000, 0x1000);
	a5_touch_alive = alive;
	if (!a5_touch_bus) {
		printf("A5XELTE TOUCH: I2C2 mapping failed\n");
		return;
	}
	con = a5touch_read(nfc, 0);
	pud = a5touch_read(nfc, 8);
	a5touch_write(nfc, 8, (pud & ~0xfu) | 0xfu);
	a5touch_write(nfc, 0, (con & ~0xffu) | 0x22u);
	delay(100);
	if (!a5touch_controller_init()) {
		a5touch_write(nfc, 0, con);
		a5touch_write(nfc, 8, pud);
		return;
	}
	if (kernel_thread_start(a5touch_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("A5XELTE TOUCH: contact thread start failed\n");
	}
}

void
a5xelte_touch_probe(void)
{
	int enabled = 1;
	int log_contacts = 0;
	int bus_trial = 0;
	vm_offset_t alive;
	vm_offset_t nfc;
	vm_offset_t mifpins;
	vm_offset_t peri;
	vm_offset_t mif;
	uint32_t pclk;
	uint32_t mifclk;
	(void)PE_parse_boot_argn("a5touchprobe", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("a5touchlog", &log_contacts, sizeof(log_contacts));
	if (!enabled) {
		return;
	}
	alive = ml_io_map(0x139b0000, 0x1000);
	nfc = ml_io_map(0x138c0000, 0x1000);
	mifpins = ml_io_map(0x104e0000, 0x1000);
	peri = ml_io_map(0x139f0000, 0x1000);
	mif = ml_io_map(0x10430000, 0x1000);
	if (!alive || !nfc || !mifpins || !peri || !mif) {
		printf("A5XELTE TOUCH: probe MMIO mapping failed\n");
		return;
	}
	pclk = a5touch_read(peri, 0x904);
	mifclk = a5touch_read(mif, 0x900);
	printf("A5XELTE TOUCH: INT GPA1 con=%#x dat=%#x; GPC3 con=%#x dat=%#x pud=%#x\n",
	    a5touch_read(alive, 0x20), a5touch_read(alive, 0x24),
	    a5touch_read(nfc, 0), a5touch_read(nfc, 4), a5touch_read(nfc, 8));
	printf("A5XELTE TOUCH: I2C2 gate=%d MIF HSI2C3 gate=%d GPM0 con=%#x dat=%#x pud=%#x\n",
	    !!(pclk & (1u << 9)), !!(mifclk & (1u << 27)),
	    a5touch_read(mifpins, 0), a5touch_read(mifpins, 4),
	    a5touch_read(mifpins, 8));
	if (pclk & (1u << 9)) {
		vm_offset_t i2c = ml_io_map(0x13850000, 0x1000);
		if (i2c) {
			printf("A5XELTE TOUCH: I2C2 con=%#x stat=%#x add=%#x lc=%#x (read only)\n",
			    a5touch_read(i2c, 0), a5touch_read(i2c, 4),
			    a5touch_read(i2c, 8), a5touch_read(i2c, 0x10));
		}
		(void)PE_parse_boot_argn("a5touchbus", &bus_trial, sizeof(bus_trial));
		if (log_contacts && (mifclk & (1u << 27))) {
			if (a5touch_power_on(mifpins)) {
				a5touch_start_early(nfc, alive);
			}
		} else if (bus_trial) {
			a5touch_chip_code_trial(nfc);
		}
	}
}
#endif
