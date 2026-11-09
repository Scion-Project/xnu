/*
 * GenericARMPE: a minimal platform expert for non-Apple ARMv8 boards.
 *
 * On Apple hardware the platform expert comes from a closed kext
 * (AppleARMPlatform). Without one, IOKit falls through to IOPanicPlatform
 * ("Unable to find driver for this platform"). This class is the least that
 * lets IOKit start on a board described by a loader-built device tree:
 * it publishes the device-tree nodes as nubs and names the machine.
 */
extern "C" {
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <kern/cpu_number.h>
}
#include <machine/machine_routines.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCPU.h>
#include <kern/clock.h>
#include <kern/thread_call.h>

#include "MiniDrivers.h"

extern "C" {
kern_return_t processor_start(processor_t processor);
kern_return_t processor_exit(processor_t processor);
extern uint32_t processor_avail_count;

/* osfmk/arm64/psci_smp.c and gic_v3_irq.c */
void psci_smp_init(void);
kern_return_t psci_smp_start_cpu(int cpu);
void psci_smp_cpu_running(void);
void psci_smp_cpu_off(void) __dead2;
void gic_v3_irq_signal(uint32_t phys_id);
}

/*
 * GenericARMCPU: registers a device-tree CPU node with the kernel. The closed
 * AppleARMCPU driver does this on Apple hardware; without it the kernel waits
 * forever for the CPU count (ml_wait_max_cpus) and never calls
 * cpu_machine_init() on the boot processor.
 *
 * The other processors are started through the firmware (PSCI CPU_ON, see
 * osfmk/arm64/psci_smp.c) and signalled with interrupts of the GIC
 * (osfmk/arm64/gic_v3_irq.c). How many there are is the device tree's and the
 * boot-arg cpus=N's to say (ml_parse_cpu_topology()); with one, nothing of
 * that is used.
 */
class GenericARMCPU : public IOCPU
{
	OSDeclareDefaultStructors(GenericARMCPU);

	uint32_t physID;        /* the device tree's "reg": the low affinity fields of the core's MPIDR */

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	virtual void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	virtual void haltCPU(void) APPLE_KEXT_OVERRIDE;
	virtual void signalCPU(IOCPU * target) APPLE_KEXT_OVERRIDE;
	virtual const OSSymbol * getCPUName(void) APPLE_KEXT_OVERRIDE;

	static int cpuNumberOfNub(IOService * nub);
	void run(void);
	kern_return_t park(void);       /* patch 1517: off while the screen is (sleep_cores=) */
	kern_return_t unpark(void);
};

OSDefineMetaClassAndStructors(GenericARMCPU, IOCPU);

/* The kernel's number for the processor of a /cpus node, -1 if it is not to be used (boot-arg cpus=N). */
int
GenericARMCPU::cpuNumberOfNub(IOService * nub)
{
	OSData * reg = OSDynamicCast(OSData, nub->getProperty("reg"));

	if (nub->getProperty("state") == NULL || reg == NULL || reg->getLength() < sizeof(uint32_t)) {
		return -1;
	}
	return ml_get_cpu_number(*(const uint32_t *)reg->getBytesNoCopy());
}

/* Registers the processor; run() starts it. */
bool
GenericARMCPU::start(IOService * provider)
{
	ml_processor_info_t info;
	perfmon_interrupt_handler_func pmi_handler = NULL;
	int cpu = cpuNumberOfNub(provider);

	if (cpu < 0 || !IOCPU::start(provider)) {
		return false;
	}
	const ml_topology_cpu_t * topology = &ml_get_topology_info()->cpus[cpu];

	physID = topology->phys_id;
	setCPUNumber((UInt32)cpu);

	bzero(&info, sizeof(info));
	info.cpu_id = (cpu_id_t)this;
	info.log_id = getCPUNumber();
	info.phys_id = physID;
	info.cluster_id = topology->cluster_id;
	info.cluster_type = topology->cluster_type;
	if (ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler) != KERN_SUCCESS) {
		IOLog("GenericARMCPU: ml_processor_register failed\n");
		return false;
	}

	setCPUState(kIOCPUStateUninitalized);
	registerService();
	IOLog("GenericARMCPU: cpu %u (phys id 0x%x) registered\n", (unsigned)getCPUNumber(), physID);
	return true;
}

/*
 * The boot processor: runs cpu_machine_init() -> initCPU(true) on it, right here.
 * Any other: cpu_start() -> startCPU(), and the new core comes to initCPU() by itself.
 */
void
GenericARMCPU::run(void)
{
	processor_start(machProcessor);
}

kern_return_t
GenericARMCPU::park(void)
{
	return processor_exit(machProcessor);
}

kern_return_t
GenericARMCPU::unpark(void)
{
	return processor_start(machProcessor);
}

/* On the processor itself, with its interrupts off, when it joins (or rejoins) the scheduler. */
void
GenericARMCPU::initCPU(bool boot)
{
	/* The interrupt controller and timer were set up in arm_init / arm_init_cpu (pe_init_fiq). */
	setCPUState(kIOCPUStateRunning);
	if (ml_get_cpu_count() > 1) {
		psci_smp_cpu_running();
		/*
		 * A processor takes no signals until it has handled an interrupt for one
		 * (cpu_signal_handler() lifts SIGPdisabled), and until then it does not go to
		 * sleep when idle either: send it its first.
		 */
		gic_v3_irq_signal(physID);
	}
}

/* On the processor itself, at the end of processor_exit(): it is to stop for good. */
void
GenericARMCPU::quiesceCPU(void)
{
	if (getCPUNumber() != (UInt32)master_cpu) {
		setCPUState(kIOCPUStateStopped);
		psci_smp_cpu_off();
	}
}

kern_return_t
GenericARMCPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
	return psci_smp_start_cpu((int)getCPUNumber());
}

void
GenericARMCPU::haltCPU(void)
{
	if (getCPUNumber() != (UInt32)master_cpu) {
		processor_exit(machProcessor);
	}
}

void
GenericARMCPU::signalCPU(IOCPU * target)
{
	gic_v3_irq_signal(((GenericARMCPU *)target)->physID);
}

const OSSymbol *
GenericARMCPU::getCPUName(void)
{
	char name[32];
	snprintf(name, sizeof(name), "Primary%u", (unsigned)getCPUNumber());
	return OSSymbol::withCString(name);
}

/*
 * Scion (patch 1517): the screen's off and on, as iOS asks for them (backboardd's power request to the display service,
 * MiniIOMobileFramebuffer.cpp), and what is done about them. Each change is logged ("GenericARMPE: the display off").
 * Boot-args, every one off by default (nothing but the log line happens without them):
 *   panel_sleep=1      the panel to sleep and back (hakios_s6e3ha8_power(), drivers/display/panel/hakios.s6e3ha8.c; with
 *                      panel=1 on the Galaxy S9; elsewhere a stub that says no)
 *   sleep_cores=1      sleep_cores_after= seconds (default 10) after the screen went off, the cores of the performance
 *                      cluster are taken off (processor_exit(): the scheduler lets them go, each switches itself off
 *                      through PSCI CPU_OFF, and the firmware may power its cluster down once all of it is off); at the
 *                      screen's on they are started again (processor_start(): PSCI CPU_ON, as at boot). 2: the
 *                      efficiency cores too. Never the boot processor, nor the boot loader's own core (core0=1).
 * Why: with the system's sleep refused (IOPMrootDomain, syssleep=), a screen-off phone stays awake with every core
 * idling in WFI, which only gates the clock: a core taken off leaks nothing. The cores come back in the time a
 * CPU_ON takes at boot (milliseconds); the screen-on does not wait for them.
 */
extern "C" int hakios_s6e3ha8_power(bool on);
extern "C" bool psci_smp_cpu_is_loaders(int cpu);
extern "C" uint32_t hakios_cpu_exit_mask;       /* osfmk/arm/cpu_common.c: the cores ml_cpu_can_exit() lets go */
extern "C" uint32_t gic_v3_spi_move(int from_cpu, int to_cpu);  /* osfmk/arm64/gic_v3_irq.c: its devices' interrupts */
extern "C" void hakios_display_power(int on);

static GenericARMCPU ** gCPUList;               /* GenericARMPE::start()'s cpus[], by cpu number */
static uint32_t gPanelSleep, gSleepCores, gSleepCoresAfter = 10;
static thread_call_t gSleepCoresCall;
static volatile int gDisplayOn = -1;            /* -1: not said yet */
static uint32_t gParked;                        /* the cores taken off (only gSleepCoresCall touches it) */

static bool
sleepCoreWanted(unsigned int number)
{
	const ml_topology_info_t * topology = ml_get_topology_info();

	if (gCPUList == NULL || gCPUList[number] == NULL || number == (unsigned int)master_cpu ||
	    psci_smp_cpu_is_loaders((int)number) || gCPUList[number]->getCPUState() != kIOCPUStateRunning) {
		return false;
	}
	return gSleepCores >= 2 || topology->cpus[number].cluster_type == CLUSTER_TYPE_P;
}

static void
sleepCoresWork(thread_call_param_t, thread_call_param_t)
{
	uint64_t t0;

	if (gDisplayOn == 0) {
		for (unsigned int number = 0; number < MAX_CPUS && number < 32 && gDisplayOn == 0; number++) {
			if ((gParked & (1u << number)) || !sleepCoreWanted(number)) {
				continue;
			}
			__atomic_fetch_or(&hakios_cpu_exit_mask, 1u << number, __ATOMIC_RELAXED);
			t0 = mach_absolute_time();
			/* first its devices' interrupts to the boot processor, which never goes */
			uint32_t moved = gic_v3_spi_move((int)number, master_cpu);
			kern_return_t kr = gCPUList[number]->park();
			if (kr == KERN_SUCCESS) {
				gParked |= 1u << number;
			} else {
				__atomic_fetch_and(&hakios_cpu_exit_mask, ~(1u << number), __ATOMIC_RELAXED);
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &t0);
			IOLog("GenericARMPE: sleep_cores: cpu %u %s (%d) in %llu us, %u of its interrupts to cpu %d first; "
			    "%u processors running\n", number, kr == KERN_SUCCESS ? "taken off" : "NOT taken off", kr, t0 / 1000, moved,
			    master_cpu, processor_avail_count);
		}
	}
	if (gDisplayOn != 0 && gParked != 0) {
		for (unsigned int number = 0; number < MAX_CPUS && number < 32; number++) {
			if (!(gParked & (1u << number))) {
				continue;
			}
			t0 = mach_absolute_time();
			__atomic_fetch_and(&hakios_cpu_exit_mask, ~(1u << number), __ATOMIC_RELAXED);
			kern_return_t kr = gCPUList[number]->unpark();
			if (kr == KERN_SUCCESS) {
				gParked &= ~(1u << number);
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &t0);
			IOLog("GenericARMPE: sleep_cores: cpu %u %s (%d) in %llu us; %u processors running\n", number,
			    kr == KERN_SUCCESS ? "started again" : "NOT started again", kr, t0 / 1000, processor_avail_count);
		}
	}
}

/* backboardd's power request (MiniIOMobileFramebuffer.cpp): 0 the screen off, anything else on */
void
hakios_display_power(int on)
{
	on = on != 0;
	if (on == gDisplayOn) {
		return;
	}
	gDisplayOn = on;
	IOLog("GenericARMPE: the display %s (iOS's power request)%s%s\n", on ? "on" : "off",
	    gPanelSleep ? "; panel_sleep=1" : "", gSleepCores ? "; sleep_cores=" : "");
	if (gPanelSleep) {
		int error = hakios_s6e3ha8_power(on);

		if (error != 0) {
			IOLog("GenericARMPE: panel_sleep: the panel says %d\n", error);
		}
	}
	if (gSleepCoresCall != NULL) {
		if (!on) {
			uint64_t deadline;

			clock_interval_to_deadline(gSleepCoresAfter, NSEC_PER_SEC, &deadline);
			thread_call_enter_delayed(gSleepCoresCall, deadline);
		} else {
			thread_call_cancel(gSleepCoresCall);
			thread_call_enter(gSleepCoresCall);
		}
	}
}

static void
screenPowerInit(void)
{
	PE_parse_boot_argn("panel_sleep", &gPanelSleep, sizeof(gPanelSleep));
	PE_parse_boot_argn("sleep_cores", &gSleepCores, sizeof(gSleepCores));
	PE_parse_boot_argn("sleep_cores_after", &gSleepCoresAfter, sizeof(gSleepCoresAfter));
	if (gSleepCores != 0 && ml_get_cpu_count() > 1) {
		gSleepCoresCall = thread_call_allocate_with_options(sleepCoresWork, NULL, THREAD_CALL_PRIORITY_KERNEL,
		    THREAD_CALL_OPTIONS_ONCE);
		IOLog("GenericARMPE: sleep_cores=%u: %s taken off %u s after the screen goes off\n", gSleepCores,
		    gSleepCores >= 2 ? "every core but the boot processor (and the loader's)" : "the performance cores",
		    gSleepCoresAfter);
	}
}

class GenericARMPE : public IODTPlatformExpert
{
	OSDeclareDefaultStructors(GenericARMPE);

public:
	virtual IOService * probe(IOService * provider, SInt32 * score) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual const char * deleteList(void) APPLE_KEXT_OVERRIDE;
	virtual const char * excludeList(void) APPLE_KEXT_OVERRIDE;
	virtual bool getMachineName(char * name, int maxLength) APPLE_KEXT_OVERRIDE;
	virtual long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
	virtual void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
};

#define super IODTPlatformExpert

OSDefineMetaClassAndStructors(GenericARMPE, IODTPlatformExpert);

IOService *
GenericARMPE::probe(IOService * provider, SInt32 * score)
{
	/*
	 * IODTPlatformExpert::probe() wants an IONameMatch against the root
	 * node; this expert is the only one built in, so it takes any board.
	 */
	if (!IOPlatformExpert::probe(provider, score)) {
		return NULL;
	}
	return this;
}

bool
GenericARMPE::start(IOService * provider)
{
	if (!super::start(provider)) {
		return false;
	}
	IOLog("GenericARMPE: platform \"%s\"\n", provider->getName());

	/*
	 * super::start() published the /cpus children as nubs. Attach a CPU driver to each
	 * one the kernel has a number for, register them all -- that settles the CPU count --
	 * and then start them, the boot processor (number master_cpu) first.
	 */
	static GenericARMCPU * cpus[MAX_CPUS];        /* (static: the slower cores may be started later, see below) */
	gCPUList = cpus;
	unsigned int count = 0;
	OSIterator * iter = getChildIterator(gIOServicePlane);
	if (iter != NULL) {
		OSObject * obj;
		while ((obj = iter->getNextObject()) != NULL) {
			IOService * nub = OSDynamicCast(IOService, obj);
			int number = nub ? GenericARMCPU::cpuNumberOfNub(nub) : -1;

			if (number < 0 || number >= MAX_CPUS || cpus[number] != NULL) {
				continue;
			}
			GenericARMCPU * cpu = new GenericARMCPU;
			if (cpu != NULL && cpu->init() && cpu->attach(nub)) {
				if (cpu->start(nub)) {
					cpus[number] = cpu;     /* kept: the kernel has its address as the cpu's id */
					count++;
					continue;
				}
				cpu->detach(nub);
			}
			if (cpu != NULL) {
				cpu->release();
			}
		}
		iter->release();
	}
	if (cpus[master_cpu] != NULL) {
		/* Unblocks everything that waits for the CPU count to be known. */
		ml_set_max_cpus(count);
		if (count > 1) {
			psci_smp_init();
		}
		/*
		 * The slower cores (an L2 access penalty, the loader's "small-core") are started 28 seconds
		 * later (boot-arg small_cores_after=S; 0: at once). With all seven from the start the lock
		 * screen came 1.5 s later than with the four big ones alone (26.5-27.0 s after the kernel
		 * began instead of 25.3-25.5: start-up's other work shares memory and cache with
		 * SpringBoard's), while that work as a whole was done earlier (34 s instead of 38). With
		 * the slower cores joining once the lock screen is there: 25.3 s and 36 s.
		 */
		uint32_t later = 28;
		static thread_call_t later_call;

		PE_parse_boot_argn("small_cores_after", &later, sizeof(later));
		cpus[master_cpu]->run();
		for (unsigned int number = 0; number < MAX_CPUS; number++) {
			if (cpus[number] != NULL && number != (unsigned int)master_cpu) {
				if (later != 0 && ml_get_topology_info()->cpus[number].l2_access_penalty != 0) {
					continue;
				}
				cpus[number]->run();
			}
		}
		if (later != 0) {
			uint64_t deadline;

			later_call = thread_call_allocate([](thread_call_param_t, thread_call_param_t) {
				for (unsigned int number = 0; number < MAX_CPUS; number++) {
					if (cpus[number] != NULL && ml_get_topology_info()->cpus[number].l2_access_penalty != 0) {
						cpus[number]->run();
					}
				}
				IOLog("GenericARMPE: the slower cores started too: %u processors running\n", processor_avail_count);
			}, NULL);
			clock_interval_to_deadline(later, NSEC_PER_SEC, &deadline);
			thread_call_enter_delayed(later_call, deadline);
		}
		if (count > 1) {
			IOLog("GenericARMPE: %u of %u processors are running\n", processor_avail_count, count);
		}
	}

	screenPowerInit();

	/* BSD looks the platform expert up by class (IOSecureBSDRoot). */
	registerService();

	/* Stand-ins for the closed graphics drivers that the Darwin window server talks to. */
	MiniIOSurfaceStart(this);
	MiniIOMobileFramebufferStart(this);
	MiniAppleKeyStoreStart();
	MiniIONetworkStackStart();
	MiniIOAESAcceleratorStart();
	return true;
}

/*
 * The time of day. There is no driver for a real-time clock here; whoever started the kernel
 * says what time it was then -- the boot-arg epoch= (seconds since 1970, put there by the
 * host scripts) or, failing that, the property boot-epoch of /chosen (the loader, from the
 * firmware's clock) -- and the kernel's own timer counts from there. Without either the
 * calendar starts in 1970, as before. A phone with a driver for its own real-time clock is asked
 * last (hakios_rtc_boot_time: the Galaxy S9's S2MPS18, rtc_pmic=1); its clock wins when valid.
 */
extern "C" uint64_t hakios_rtc_boot_time(uint64_t epoch);

static long gBootEpoch;         /* the calendar time at which the system timer read zero */
static bool gBootEpochKnown;

static void
findBootEpoch(void)
{
	uint64_t epoch = 0;

	if (gBootEpochKnown) {
		return;
	}
	gBootEpochKnown = true;
	if (!PE_parse_boot_argn("epoch", &epoch, sizeof(epoch)) || epoch == 0) {
		IORegistryEntry * chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
		OSData * data = chosen ? OSDynamicCast(OSData, chosen->getProperty("boot-epoch")) : NULL;

		if (data != NULL && data->getLength() == sizeof(epoch)) {
			memcpy(&epoch, data->getBytesNoCopy(), sizeof(epoch));
		}
		OSSafeReleaseNULL(chosen);
	}
	epoch = hakios_rtc_boot_time(epoch);    /* the phone's own clock, when it has a driver and it is valid */
	if (epoch != 0) {
		clock_sec_t secs;
		clock_usec_t usecs;

		clock_get_system_microtime(&secs, &usecs);
		gBootEpoch = (long) epoch - (long) secs;
	}
}

long
GenericARMPE::getGMTTimeOfDay(void)
{
	clock_sec_t secs;
	clock_usec_t usecs;

	findBootEpoch();
	if (gBootEpoch == 0) {
		return 0;
	}
	clock_get_system_microtime(&secs, &usecs);
	return gBootEpoch + (long) secs;
}

void
GenericARMPE::setGMTTimeOfDay(long secs)
{
	clock_sec_t uptime;
	clock_usec_t usecs;

	clock_get_system_microtime(&uptime, &usecs);
	gBootEpoch = secs - (long) uptime;
	gBootEpochKnown = true;
}

/* Device-tree nodes to drop entirely. */
const char *
GenericARMPE::deleteList(void)
{
	return "('packages', 'psuedo-usb', 'psuedo-hid', 'multiboot', 'rtas')";
}

/* Top-level nodes that describe boot state rather than devices: no nubs for these. */
const char *
GenericARMPE::excludeList(void)
{
	return "('chosen', 'memory', 'options', 'aliases', 'defaults', 'pram', 'cpus')";
}

/*
 * hw.machine. On Apple's devices it is the product type ("iPhone10,3"), from the device tree's
 * "model", and Darwin decides from it what kind of device it is on: with anything it does not know
 * MobileGestalt settles for an iPod touch.
 */
bool
GenericARMPE::getMachineName(char * name, int maxLength)
{
	uint32_t real = 1;

	/*
	 * hw.machine is the model the loader put into the device tree, "iPhone10,3". It has to
	 * begin with "iPhone": CoreFoundation picks the device-specific variants of resources
	 * (Something~iphone.plist, Key~iphone in an Info.plist) by this string, and for a name it
	 * does not know it picks none at all -- Control Center, for one, then lays its modules out
	 * without their sizes. (boot-arg machine_name=0: the plain "arm64" this used to be.)
	 */
	PE_parse_boot_argn("machine_name", &real, sizeof(real));
	if (!real || !IODTPlatformExpert::getMachineName(name, maxLength) || name[0] == 0) {
		strlcpy(name, "arm64", maxLength);
	}
	return true;
}
