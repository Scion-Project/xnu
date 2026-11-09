/*
 * Exynos7580 ALIVE GPIO keys for the SM-A510F.
 *
 * The working Linux device tree locates Power/Volume Up/Volume Down/Home at
 * GPA2 pins 7/0/1/5, all active low.  Poll only GPA2DAT; boot firmware owns
 * the pin mux and pull-ups.  No ALIVE or interrupt-controller writes occur.
 *
 * HEROLTE (Galaxy S7, Exynos 8890, from its BOOT DTB gpio_keys): ALIVE at
 * 0x10580000, Power GPA2 pin 4, Volume Up/Down and Home GPA0 pins 3/4/5,
 * active low.  S-Boot reads them too, so their mux is already set; the two
 * DAT registers are sampled together (bits 0-7 GPA0, 8-15 GPA2).
 */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_hid_key(int key, int down);

#if defined(ARM64_BOARD_CONFIG_STARLTE)
#define A5_ALIVE_PA 0x14050000u
#define A5_GPA0_CON 0x20u
#define A5_GPA0_DAT 0x24u
#define A5_GPA0_PUD 0x28u
#define A5_GPA2_CON 0x60u
#define A5_GPA2_DAT 0x64u
#define A5_GPA2_PUD 0x68u
#else
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#define A5_ALIVE_PA 0x10580000u
#define A5_GPA0_CON 0x00u
#define A5_GPA0_DAT 0x04u
#define A5_GPA0_PUD 0x08u
#else
#define A5_ALIVE_PA 0x139b0000u
#endif
#define A5_GPA2_CON 0x40u
#define A5_GPA2_DAT 0x44u
#define A5_GPA2_PUD 0x48u
#endif

static vm_offset_t a5_alive;
/* Last Power press (mach_absolute_time); SweetFramebuffer.cpp reads it. */
extern uint64_t a5xelte_power_pressed_at;
uint64_t a5xelte_power_pressed_at;
static int a5_keys_sleep;
static bool a5_keys_started;

static uint32_t
a5_keys_read(uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(a5_alive + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
a5_keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&a5_keys_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Key order of sweet_hid_key(): power, volume up, volume down, home. */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
static uint16_t
a5_keys_sample(void)
{
	return (uint16_t)((a5_keys_read(A5_GPA0_DAT) & 0xff) |
	       ((a5_keys_read(A5_GPA2_DAT) & 0xff) << 8));
}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* Bixby (GPA0-6) stands in for Home. */
static const uint8_t a5_key_pins[4] = {8 + 4, 3, 4, 6};
#else
static const uint8_t a5_key_pins[4] = {8 + 4, 3, 4, 5};
#endif
#else
static uint16_t
a5_keys_sample(void)
{
	return (uint16_t)(a5_keys_read(A5_GPA2_DAT) & 0xff);
}
static const uint8_t a5_key_pins[4] = {7, 0, 1, 5};
#endif

static void
a5_keys_thread(void *arg, wait_result_t result)
{
	uint16_t stable = a5_keys_sample();
	uint16_t previous = stable;
	unsigned repeated = 0;
	(void)arg;
	(void)result;
	for (;;) {
		uint16_t sample = a5_keys_sample();
		if (sample == previous) {
			if (repeated < 2) {
				repeated++;
			}
		} else {
			previous = sample;
			repeated = 0;
		}
		if (repeated == 2 && stable != sample) {
			for (unsigned key = 0; key < 4; key++) {
				uint16_t bit = (uint16_t)(1u << a5_key_pins[key]);
				if ((stable ^ sample) & bit) {
					int down = !(sample & bit);
					printf("A5XELTE KEYS: key=%u down=%d gpio=%#x\n",
					    key, down, sample);
					if (key == 0 && down) {
						a5xelte_power_pressed_at = mach_absolute_time();
					}
					sweet_hid_key((int)key, down);
				}
			}
			stable = sample;
		}
		a5_keys_wait();
	}
}

void
a5xelte_keys_init(void)
{
	thread_t thread;
	int enabled = 1;
	if (a5_keys_started) {
		return;
	}
	a5_keys_started = true;
	(void)PE_parse_boot_argn("a5keys", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("A5XELTE KEYS: disabled\n");
		return;
	}
	a5_alive = ml_io_map(A5_ALIVE_PA, 0x1000);
	if (!a5_alive) {
		printf("A5XELTE KEYS: ALIVE mapping failed\n");
		return;
	}
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	printf("A5XELTE KEYS: GPA0 con=%#x dat=%#x pud=%#x (read only)\n",
	    a5_keys_read(A5_GPA0_CON), a5_keys_read(A5_GPA0_DAT),
	    a5_keys_read(A5_GPA0_PUD));
#endif
	printf("A5XELTE KEYS: GPA2 con=%#x dat=%#x pud=%#x (read only)\n",
	    a5_keys_read(A5_GPA2_CON), a5_keys_read(A5_GPA2_DAT),
	    a5_keys_read(A5_GPA2_PUD));
	kern_return_t kr = kernel_thread_start(a5_keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("A5XELTE KEYS: thread start failed %d\n", kr);
	}
}
#endif
