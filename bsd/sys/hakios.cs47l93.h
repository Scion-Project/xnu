/*
 * PORT NOTE (Scion's iOS 27 tree, the fork's scion/xnu-12377, xnu-12377 STARLTE): from Scion-Project/scion-kernel xnu/files/bsd/sys/
 * hakios.cs47l93.h (APSL; HakiOS/Scion), restored to accompany the CS47L93 codec driver routed in via
 * profiles/ios-27.copy (drivers/audio/codec/hakios.cs47l93.c -> bsd/dev/hakios.cs47l93.c). The file previously
 * removed with the in-fork copy; now the sk copy is the source of truth (dual-XNU guard).
 */
/*
 * CS47L93 (Cirrus Logic Madera) codec driver interface for the Galaxy S9.
 * Microphone (IN3L) and headphone (OUT2 / jack) support via SPI on USI01.
 */
#ifndef _SYS_HAKIOS_CS47L93_H_
#define _SYS_HAKIOS_CS47L93_H_

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Device ID returned from register 0x0000 (SOFTWARE_RESET) for the CS47L92 and CS47L93. */
#define HAKIOS_CS47L93_ID       0x6371u

void    hakios_cs47l93_init(void);
void    hakios_cs47l93_quiesce(void);
void    hakios_cs47l93_console(const char *arg);

__END_DECLS

#endif /* _SYS_HAKIOS_CS47L93_H_ */
