/*
 * Ported to the Starbite iOS 27 (xnu-12377) STARLTE tree from HakiOS/Scion (Scion-Project/scion-kernel,
 * drivers/power/exynos/hakios.exynos_tmu.c, APSL), keeping Scion's notes below. What differs here:
 *   - Steps 1 and 2 only (reads; kern.hakios.temp.* and the functions the CPU governor calls). Step 3 (ending a hot run
 *     through the loader, or PS_HOLD with tmu_off=1) is not here: this tree has no loader_return and none of the drivers
 *     it stops first. tmu=3 is taken as tmu=2: NOTHING STOPS A HOT RUN in this tree.
 *   - No writes at all: tmu_enable= and tmu_ungate= are not here. A block the boot left stopped reads nothing; a closed
 *     PCLK gate means the TMU's registers are never touched.
 *   - No battery temperature (no battery driver in this tree): kern.hakios.temp.battery_c is not published.
 *   - No kern.hakios.thermal_* (the POCO's names; hakios.thermal_provider.c is not in this tree).
 *   - Started from IOKitBSDInit (just before the CPU governor, hakios_exynos_cpufreq_gov_start), not by STARTUP().
 *   - The registers' facts and arithmetic: bsd/sys/hakios.exynos_tmu.h (Scion's, unchanged).
 *   The rest of Scion's notes follow, step 3 and the writes included, as they were.
 *
 * The Galaxy S9's temperatures: the Exynos 9810's TMU, read by the kernel itself, published, and a stop for a phone that
 * gets too hot. Nothing without boot-arg tmu=N, and then only its first N steps, each said in the log before it is taken.
 *
 * Why. Under Android the S9 is kept cool by Linux's thermal framework and Samsung's APM firmware (ACPM): they slow the
 * big cores from 83 degrees, take them away at 96, slow the GPU from 88. HakiOS has none of that. With bootcpu=4 cpus=8
 * the four M3s run at the loader's clock, the Mali-G72 at whatever mali=N set, and nothing would notice the heat. This
 * driver is the S9's counterpart of the POCO's drivers/power/hakios.thermal.c (whose sysctls it also fills, so that
 * whatever reads those on the POCO reads the same here) and battguard.c's way of ending a run.
 *
 * The hardware (hakios.exynos_tmu.h has every register, field and line of the BSP it comes from). Two TMU blocks:
 * TMU_TOP at 0x10070000 (the BIG zone: sensors 1, 2, 3, 4, 6; LITTLE: sensor 5) and TMU_SUB at 0x10074000 (G3D: sensor 1;
 * ISP: sensor 2). Each sensor gives a 9-bit code, all the time while the block's CORE_EN is set, in CURRENT_TEMP
 * (0x40/0x44/0x48). The codes are made degrees with each sensor's fuses, which the block shows as its TRIMINFO registers
 * (one per sensor: [8:0] the code at 25 degrees, [17:9] the code at 85; bit 23 of TRIMINFO0 says one- or two-point
 * calibration), as exynos9810_tmu_initialize() and code_to_temp_with_sensorinfo() do. Android itself never reads the codes
 * this way in normal use: with CONFIG_EXYNOS_ACPM_THERMAL (the S9's defconfig) the temperature comes from the APM firmware
 * over IPC (exynos9810_tmu_read -> exynos_acpm_tmu_set_read_temp). The register path used here is the BSP's own all_temp
 * sysfs file (all_temp_show), which reads the same CURRENT_TEMP registers with the same arithmetic. The APM's IPC is not
 * used: it needs Linux's TMU_IPC_INIT first, and its "balance" mode for BIG is the firmware's and not written down; this
 * driver takes the hottest sensor of each zone, which is never cooler than Android's figure.
 *
 * The clock. The TMU's bus clock is CMU_PERIS's BUSIF_TMU PCLK (gate +0x2014, Q-channel +0x3004 of 0x10020000;
 * cmucal-sfr.c:36, 1656, 1675, 7016-7018, 7070-7073). A gate forced shut (MANUAL set, CG_VAL clear) would make a read of
 * the TMU stop the bus, so step 1 reads the gate first and does not touch the TMU behind a closed one (tmu_ungate=1 opens
 * it, read back).
 *
 * THE STEPS (tmu=N does the first N):
 *   1  reads only, once at start and then every tmu_poll ms: the CMU gate; of each block CONTROL (CORE_EN, THERM_TRIP_EN,
 *      TRIP_MODE), CONTROL1, STATUS, AVG_CON, TRIM0, EMUL_CON; of each sensor of the four zones (and sensor 0) its TRIMINFO,
 *      its calibration, its code and temperature, its RISE7/RISE6 thresholds and interrupt enables as the boot left them
 *      (Android writes its "hot" trip, 115 degrees, as RISE7: a value there says the boot loader armed the block's own
 *      thermal trip); then a summary line every tmu_log seconds and at once when the hottest zone passes a multiple of 5
 *      from 60, or the point where Android would start to cool that zone
 *   2  publishes: sysctl kern.hakios.temp.* (big_mc, little_mc, g3d_mc, isp_mc, max_mc in millidegrees; hottest, sensors,
 *      reads, battery_c) and the POCO's kern.hakios.thermal_max_mc, thermal_hottest, thermal_sensors, thermal_reads, and
 *      hakios_thermal_max_mc() for other drivers; -273000 is "not read"
 *   3  acts: (not in this port; see above)
 *
 * Status (Scion): run on the phone with tmu=3 since 1 November (the zones read, kern.hakios.temp; the S9's default).
 * Not known: whether S-Boot leaves the TMU running and its PCLK open (step 1 says), whether the APM, which owns the TMU
 * under Android, changes its registers while HakiOS runs, and the BIG zone's "balance" mode.
 * Status (this port): built and linked; not yet run in this tree (S-Boot -> shim -> XNU, not Scion's loader).
 *
 * Boot-args (this port):
 *   tmu=<1-2>            the steps: 1 reads, 2 publishes (0, or none: nothing mapped, nothing read; 3 is taken as 2)
 *   tmu_poll=<ms>        how often to read (200 to 60000; 1000, the BIG zone's polling-delay)
 *   tmu_log=<s>          a summary this often (0: only on a rise; 60)
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <stdint.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_soc.h>
#include <sys/hakios.exynos_tmu.h>
#include <sys/hakios.log.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

SYSCTL_DECL(_kern_hakios);

#define CMU_PERIS_PA            ((uint32_t)EXYNOS_CMU_PERIS_PA)     /* cmucal-sfr.c:36 */
#define CMU_PERIS_TMU_GATE      EXYNOS_TMU_CMU_PERIS_TMU_GATE         /* :1656 CLK_CON_GAT_GOUT_BLK_PERIS_UID_BUSIF_TMU_IPCLKPORT_PCLK */
#define CMU_PERIS_TMU_QCH       EXYNOS_TMU_CMU_PERIS_TMU_QCH         /* :1675 QCH_CON_BUSIF_TMU_QCH */
#define GATE_MANUAL             (1u << 20)      /* :7017 */
#define GATE_CG_VAL             (1u << 21)      /* :7016 */
#define NOT_READ_MC             (-273000)

static vm_offset_t block_va[2], cmu_va;
static const uint32_t block_pa[2] = { TMU_TOP_PA, TMU_SUB_PA };
static const char *const block_name[2] = { "TMU_TOP", "TMU_SUB" };
static struct tmu_cal cal[2][8];
static uint32_t steps, poll_ms = 1000, log_seconds = 60;
static uint64_t last_log;
static volatile uint64_t reads;
static int last_step;
static bool warned_cools[TMU_NZONES];
static thread_call_t call;

/* step 2's values (sysctl) */
static int temp_zone_mc[TMU_NZONES] = { NOT_READ_MC, NOT_READ_MC, NOT_READ_MC, NOT_READ_MC };
static int temp_max_mc = NOT_READ_MC, temp_reads;
static char temp_hottest[16] = "", temp_sensors[400] = "";

SYSCTL_NODE(_kern_hakios, OID_AUTO, temp, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Exynos 9810's TMU (bsd/dev/hakios.exynos_tmu.c, tmu=2)");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, big_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[0], 0, "the big cores' hottest sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, little_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[1], 0, "the little cores' sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, g3d_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[2], 0, "the GPU's sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, isp_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_zone_mc[3], 0, "the camera ISP's sensor, millidegrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, max_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_max_mc, 0, "the hottest zone, millidegrees (-273000: none read)");
SYSCTL_STRING(_kern_hakios_temp, OID_AUTO, hottest, CTLFLAG_RD | CTLFLAG_LOCKED, temp_hottest, 0, "the hottest zone's name");
SYSCTL_STRING(_kern_hakios_temp, OID_AUTO, sensors, CTLFLAG_RD | CTLFLAG_LOCKED, temp_sensors, 0,
    "every sensor in degrees: \"big1=41.0 big2=40.5 ...\"");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, reads, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_reads, 0, "how many times the sensors were read");

/*
 * Every zone's last reading for other drivers (the CPU governor, bsd/dev/hakios.exynos_cpufreq.c), whatever the step:
 * the zones in tmu_zones' order (0 BIG, 1 LITTLE, 2 G3D, 3 ISP), millidegrees, NOT_READ_MC before the first reading.
 * Asked directly: the governor read kern.hakios.temp.* through kernel_sysctlbyname at first, and got nothing on the phone.
 */
static volatile int32_t last_zone_mc[TMU_NZONES] = { NOT_READ_MC, NOT_READ_MC, NOT_READ_MC, NOT_READ_MC };
static volatile uint64_t last_zone_at;

int32_t hakios_exynos_tmu_zone_mc(unsigned zone);

int32_t
hakios_exynos_tmu_zone_mc(unsigned zone)
{
	return zone < TMU_NZONES ? last_zone_mc[zone] : NOT_READ_MC;
}

/* for the CPU governor's fall-back: how long ago the zones were last read, ms */
uint64_t hakios_exynos_tmu_age_ms(void);

uint64_t
hakios_exynos_tmu_age_ms(void)
{
	uint64_t ns;

	if (last_zone_at == 0) {
		return UINT64_MAX;      /* (Starbite) never read: as old as can be */
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - last_zone_at, &ns);
	return ns / NSEC_PER_MSEC;
}

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
text_of(char *out, size_t size, int32_t mc)
{
	int32_t tenth = mc / 100, whole = tenth / 10, frac = tenth % 10;

	snprintf(out, size, "%s%d.%d", (tenth < 0 && whole == 0) ? "-" : "", whole, frac < 0 ? -frac : frac);
}

static uint32_t
code_of(unsigned block, unsigned sensor)
{
	return (rd(block_va[block], tmu_temp_reg(sensor)) >> tmu_temp_shift(sensor)) & 0x1ffu;
}

/* step 1's look at what the boot left, once */
static void
describe(void)
{
	char one[16], two[16];

	for (unsigned b = 0; b < 2; b++) {
		vm_offset_t va = block_va[b];
		uint32_t ctrl = rd(va, TMU_CONTROL), t0 = rd(va, TMU_TRIMINFO(0));

		say("tmu: 1: %s %#x: CONTROL %08x (CORE_EN %u, THERM_TRIP_EN %u, TRIP_MODE %u, VREF %u, SLOPE %u), CONTROL1 %08x "
		    "(NUM_PROBE %u, LPI %u), STATUS %08x, AVG_CON %08x, COUNTER %08x/%08x, TRIM0 %08x, EMUL_CON %08x; %s-point calibration\n",
		    block_name[b], block_pa[b], ctrl, ctrl & TMU_CTRL_CORE_EN ? 1 : 0, ctrl & TMU_CTRL_TRIP_EN ? 1 : 0,
		    TMU_CTRL_TRIP_MODE(ctrl), (ctrl >> TMU_CTRL_VREF_SHIFT) & 0x1f, (ctrl >> TMU_CTRL_SLOPE_SHIFT) & 0xf,
		    rd(va, TMU_CONTROL1), TMU_CON1_NUM_PROBE(rd(va, TMU_CONTROL1)), rd(va, TMU_CONTROL1) & TMU_CON1_LPI ? 1 : 0,
		    rd(va, TMU_STATUS), rd(va, TMU_AVG_CON), rd(va, TMU_COUNTER_VALUE0), rd(va, TMU_COUNTER_VALUE1), rd(va, TMU_TRIM0),
		    rd(va, TMU_EMUL_CON), TMU_TRIM_CAL_TYPE(t0) ? "two" : "one");
		if (rd(va, TMU_EMUL_CON) & TMU_EMUL_ENABLE) {
			say("tmu: 1: %s: EMULATION IS ON: its readings are a made-up temperature, not the chip's\n", block_name[b]);
		}
		if (!(ctrl & TMU_CTRL_CORE_EN)) {
			say("tmu: 1: %s: CORE_EN is clear: the boot left it stopped and this port never starts it; its zones read nothing\n",
			    block_name[b]);
		}
		for (unsigned s = 0; s < 8; s++) {
			uint16_t used = 0;

			for (unsigned z = 0; z < TMU_NZONES; z++) {
				if (tmu_zones[z].block == b && (tmu_zones[z].sensors & (1u << s))) {
					used = 1;
				}
			}
			if (!used && s != 0) {
				continue;
			}
			uint32_t trim = rd(va, TMU_TRIMINFO(s)), code = code_of(b, s), thd = rd(va, tmu_thd_reg(s));
			uint32_t r7 = (thd >> 16) & 0x1ff, r6 = thd & 0x1ff;
			const struct tmu_cal *c = &cal[b][s];

			text_of(one, sizeof(one), tmu_code_to_mc(c, code));
			text_of(two, sizeof(two), tmu_code_to_mc(c, r7));
			say("tmu: 1: %s sensor %u%s: TRIMINFO %08x (25 C = %u, 85 C = %u%s), code %u = %s C; RISE7 %u (%s C) RISE6 %u, "
			    "INTEN %08x\n", block_name[b], s, used ? "" : " (in no zone)", trim, c->error1, c->error2,
			    c->fell_back == 1 ? ", fuse empty: the dt's 55" : c->fell_back == 2 ? ", second point unusable: one-point" : "",
			    code, one, r7, r7 ? two : "-", r6, rd(va, tmu_inten_reg(s)));
		}
	}
}

int32_t hakios_exynos_tmu_zone_now_mc(unsigned zone);

/*
 * A zone's hottest sensor, read now, for the CPU governor above its all-core rates: two M3s at 2314000 kHz go from 46 C to 92 C
 * in the few seconds that a reading a second sees five of (phone 2, 5 November). Registers only; nothing changes.
 */
int32_t
hakios_exynos_tmu_zone_now_mc(unsigned zone)
{
	int32_t hottest = NOT_READ_MC;

	if (zone >= TMU_NZONES || reads == 0) {
		return NOT_READ_MC;
	}
	for (unsigned s = 0; s < 8; s++) {
		if (tmu_zones[zone].sensors & (1u << s)) {
			uint32_t code = code_of(tmu_zones[zone].block, s);
			int32_t mc = tmu_code_to_mc(&cal[tmu_zones[zone].block][s], code);

			if (tmu_reading_ok(code, mc) && mc > hottest) {
				hottest = mc;
			}
		}
	}
	return hottest;
}

static void
look(void)
{
	int32_t zone_mc[TMU_NZONES], hottest = INT32_MIN;
	unsigned hot = 0, good = 0;
	char list[sizeof(temp_sensors)], one[16], item[32];

	list[0] = 0;
	for (unsigned z = 0; z < TMU_NZONES; z++) {
		const struct tmu_zone *zone = &tmu_zones[z];

		zone_mc[z] = INT32_MIN;
		for (unsigned s = 0; s < 8; s++) {
			if (!(zone->sensors & (1u << s))) {
				continue;
			}
			uint32_t code = code_of(zone->block, s);
			int32_t mc = tmu_code_to_mc(&cal[zone->block][s], code);

			if (!tmu_reading_ok(code, mc)) {
				snprintf(item, sizeof(item), "%s%u=? ", zone->name, s);
			} else {
				text_of(one, sizeof(one), mc);
				snprintf(item, sizeof(item), "%s%u=%s ", zone->name, s, one);
				good++;
				if (mc > zone_mc[z]) {
					zone_mc[z] = mc;
				}
			}
			strlcat(list, item, sizeof(list));
		}
		if (zone_mc[z] != INT32_MIN && zone_mc[z] > hottest) {
			hottest = zone_mc[z];
			hot = z;
		}
	}

	reads++;
	for (unsigned z = 0; z < TMU_NZONES; z++) {
		last_zone_mc[z] = zone_mc[z] == INT32_MIN ? NOT_READ_MC : zone_mc[z];
	}
	last_zone_at = mach_absolute_time();
	if (steps >= 2) {
		for (unsigned z = 0; z < TMU_NZONES; z++) {
			temp_zone_mc[z] = zone_mc[z] == INT32_MIN ? NOT_READ_MC : zone_mc[z];
		}
		temp_reads = (int)reads;
		strlcpy(temp_sensors, list, sizeof(temp_sensors));
		if (good != 0) {
			temp_max_mc = hottest;
			strlcpy(temp_hottest, tmu_zones[hot].name, sizeof(temp_hottest));
		}
	}
	if (good == 0) {
		if (reads == 1 || reads % 60 == 0) {
			say("tmu: no sensor gives a reading (%s): is the block running? (step 1's CORE_EN)\n", list);
		}
		return;
	}

	uint64_t now = mach_absolute_time(), ns;
	int step = hottest >= 60000 ? hottest / 5000 : 0;       /* a step each 5 degrees from 60 */

	absolutetime_to_nanoseconds(now - last_log, &ns);
	text_of(one, sizeof(one), hottest);
	if (step > last_step || last_log == 0 || (log_seconds != 0 && ns / NSEC_PER_SEC >= log_seconds)) {
		say("tmu: %u sensors read; hottest %s at %s C; %s\n", good, tmu_zones[hot].name, one, list);
		last_log = now;
	}
	last_step = step;
	for (unsigned z = 0; z < TMU_NZONES; z++) {
		if (zone_mc[z] != INT32_MIN && zone_mc[z] >= tmu_zones[z].android_cools_mc && !warned_cools[z]) {
			warned_cools[z] = true;
			text_of(one, sizeof(one), zone_mc[z]);
			say("tmu: WARNING: %s at %s C: Android would be cooling it from %d C; only the CPU governor (cpufreq_gov=1) "
			    "does anything here, and nothing stops the run\n", tmu_zones[z].name, one, tmu_zones[z].android_cools_mc / 1000);
		} else if (zone_mc[z] != INT32_MIN && zone_mc[z] < tmu_zones[z].android_cools_mc - 5000) {
			warned_cools[z] = false;
		}
	}
}

static void
tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	look();
	clock_interval_to_deadline(poll_ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}

/* (Starbite) from IOKitBSDInit, before hakios_exynos_cpufreq_gov_start(); with no tmu= (or tmu=0) nothing at all */
void hakios_exynos_tmu_start(void);

void
hakios_exynos_tmu_start(void)
{
	uint32_t value = 0;

	if (!PE_parse_boot_argn("tmu", &steps, sizeof(steps)) || steps == 0) {
		steps = 0;
		return;
	}
	steps = steps > 2 ? 2 : steps;
	if (PE_parse_boot_argn("tmu_poll", &value, sizeof(value)) && value >= 200 && value <= 60000) {
		poll_ms = value;
	}
	if (PE_parse_boot_argn("tmu_log", &value, sizeof(value)) && value <= 86400) {
		log_seconds = value;
	}
	say("tmu: steps 1-%u (1 reads, 2 publishes kern.hakios.temp); every %u ms; NOTHING STOPS A HOT RUN (no step 3 in this tree)\n",
	    steps, poll_ms);

	say("tmu: 1: reading CMU_PERIS's TMU gate (%#x +%#x) and Q-channel (+%#x)\n", CMU_PERIS_PA, CMU_PERIS_TMU_GATE, CMU_PERIS_TMU_QCH);
	cmu_va = ml_io_map(CMU_PERIS_PA, 0x4000);
	if (cmu_va == 0) {
		say("tmu: could not map CMU_PERIS; off\n");
		return;
	}
	uint32_t gate = rd(cmu_va, CMU_PERIS_TMU_GATE), qch = rd(cmu_va, CMU_PERIS_TMU_QCH);
	bool closed = (gate & GATE_MANUAL) && !(gate & GATE_CG_VAL);

	say("tmu: 1: gate %08x (MANUAL %u, CG_VAL %u)%s, Q-channel %08x (ENABLE %u, CLOCK_REQ %u)\n", gate, gate & GATE_MANUAL ? 1 : 0,
	    gate & GATE_CG_VAL ? 1 : 0, closed ? ": CLOSED" : ": open", qch, qch & 1, (qch >> 1) & 1);
	if (closed) {
		say("tmu: the TMU's clock is shut: its registers are not touched (this port never opens it); off\n");
		return;
	}
	for (unsigned b = 0; b < 2; b++) {
		block_va[b] = ml_io_map(block_pa[b], TMU_SIZE);
		if (block_va[b] == 0) {
			say("tmu: could not map %s at %#x; off\n", block_name[b], block_pa[b]);
			return;
		}
	}
	for (unsigned b = 0; b < 2; b++) {
		uint32_t t0 = rd(block_va[b], TMU_TRIMINFO(0));

		for (unsigned s = 0; s < 8; s++) {
			cal[b][s] = tmu_cal_of(rd(block_va[b], TMU_TRIMINFO(s)), t0);
		}
	}
	describe();
	if (steps >= 2) {
		say("tmu: 2: publishing kern.hakios.temp.*\n");
	}
	call = thread_call_allocate(tick, NULL);
	if (call == NULL) {
		say("tmu: no thread call; off\n");
		return;
	}
	tick(NULL, NULL);
}
#endif /* ARM64_BOARD_CONFIG_STARLTE */
