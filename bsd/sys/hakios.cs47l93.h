/*
 * The Galaxy S9's audio codec, a Cirrus Logic CS47L93 (the Madera family) on spi@10470000 (USI01 of PERIC0):
 * drivers/audio/codec/hakios.cs47l93.c, boot-arg cs47l93=N. The headphones, the jack and the microphones go through it; the
 * speakers do not (ABOX's UAIF1 drives the MAX98512s directly).
 */
#ifndef _SYS_HAKIOS_CS47L93_H_
#define _SYS_HAKIOS_CS47L93_H_

#include <stdint.h>

#define HAKIOS_CS47L93_ID               0x6371  /* MADERA_SOFTWARE_RESET (0x0000) reads it: CS47L92 and CS47L93 */

#ifdef KERNEL
void hakios_cs47l93_console(const char *arg);  /* the USB console's "codec" (hakios.dwc3.c) */
void hakios_cs47l93_quiesce(void);             /* loader_return(): the outputs off, the codec held in reset */
#endif

#endif /* _SYS_HAKIOS_CS47L93_H_ */
