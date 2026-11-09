/* PORT NOTE: Scion-Project/scion-kernel xnu/files/bsd/sys/hakios.max98512.h (APSL; HakiOS/Scion); hakios_max98512_init() added. */
/*
 * The Galaxy S9's speaker amplifiers (drivers/audio/codec/hakios.max98512.c): two Maxim MAX98512s on the HSI2C of USI07 of
 * PERIC1, fed by UAIF1 of ABOX over I2S. The driver starts itself (boot-arg max98512=N); what it offers the rest of the
 * kernel is the hook below, through which ABOX's driver turns the amplifiers on once UAIF1's clocks run and off before
 * they stop.
 *
 * The hook is ABOX's, declared in sys/hakios.exynos_abox.h; a kernel without ABOX's driver has a stand-in that keeps
 * nothing (drivers/core/hakios.stubs.c).
 */
#ifndef _SYS_HAKIOS_MAX98512_H_
#define _SYS_HAKIOS_MAX98512_H_

#include <stdint.h>

#include <sys/hakios.exynos_abox.h>

void hakios_max98512_init(void);       /* (port) boot-arg max98512=N, from IOKitBSDInit.cpp */
void hakios_max98512_quiesce(void);    /* loader_return(): both amplifiers shut down, if they were started */

/*
 * What the MAX98512 driver's hook asks of its caller: start() and stop() from a thread (they take a lock and talk over
 * I2C), never at once from two; start() takes some tens of milliseconds (both amplifiers reset and set up anew), stop()
 * about 16 (the amplifiers ramp down on UAIF1's clocks, so the clocks must still run until it returns). start() takes
 * 48000 Hz, two channels (I2S: channel 0 the top speaker, 1 the bottom one) of 16, 24 or 32 bits, and refuses the rest.
 * hakios_abox_set_amp() may be called before ABOX's driver has started: it is to keep the pointer, nothing more.
 */

#endif /* _SYS_HAKIOS_MAX98512_H_ */
