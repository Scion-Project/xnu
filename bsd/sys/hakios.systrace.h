/*
 * Boot-arg systrace=1|2: BSD system calls logged as they finish (drivers/core/hakios.systrace.c). The check
 * stays in the caller, so a kernel with it off pays one load and a branch per call.
 */
#ifndef _SYS_HAKIOS_SYSTRACE_H_
#define _SYS_HAKIOS_SYSTRACE_H_

#include <stdint.h>
#include <sys/cdefs.h>

struct proc;
struct uthread;

extern uint32_t hakios_systrace_level;
void hakios_systrace_report(struct proc *proc, unsigned short syscode, struct uthread *uthread, int error);

#define HAKIOS_SYSTRACE(proc, syscode, uthread, error) \
	do { \
		if (__improbable(hakios_systrace_level != 0)) { \
			hakios_systrace_report((proc), (syscode), (uthread), (error)); \
		} \
	} while (0)

#endif /* _SYS_HAKIOS_SYSTRACE_H_ */
