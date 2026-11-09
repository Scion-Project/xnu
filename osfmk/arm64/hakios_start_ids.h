/* included by start.s in reset_vector, after the MPIDR is read into x15: the S9's M3 bring-up diagnostic (this core's ID registers, parked cores) */
	// HakiOS diagnostic (S9 M3 bring-up): this core's ID registers, MMU off, to the start of exynos-snapshot's
	// log_kernel (0xFDA10000) at 32 * Affinity1: MMFR0, MIDR, MPIDR, PFR0 -- read back from TWRP's /proc/last_kmsg
	movz	x2, #0xFDA1, lsl #16
	ubfx	x3, x15, #8, #4
	add		x2, x2, x3, lsl #5
	mrs		x3, ID_AA64MMFR0_EL1
	str		x3, [x2]
	mrs		x3, MIDR_EL1
	str		x3, [x2, #8]
	str		x15, [x2, #16]
	mrs		x3, ID_AA64PFR0_EL1
	str		x3, [x2, #24]
	dsb		sy
	// a big core (MT clear, Affinity1 1) parks here when asked (boot-arg m3_ids=1): its IDs read by the boot core
	adrp	x2, EXT(hakios_m3_park)@page
	add		x2, x2, EXT(hakios_m3_park)@pageoff
	ldr		w2, [x2]
	cbz		w2, Lm3_nopark
	tbnz	x15, #24, Lm3_nopark
	ubfx	x3, x15, #8, #4
	cmp		x3, #1
	b.ne	Lm3_nopark
Lm3_park:
	wfe
	b		Lm3_park
Lm3_nopark:
