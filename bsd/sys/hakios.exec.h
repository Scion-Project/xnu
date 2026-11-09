/*
 * What kern_exec.c asks of drivers/core/hakios.exec.c: the exec and posix_spawn bring-up aids and the persona credential.
 */
#ifndef _SYS_HAKIOS_EXEC_H_
#define _SYS_HAKIOS_EXEC_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/cdefs.h>
#include <sys/proc.h>
#include <sys/kauth.h>

struct image_params;

/* boot-arg systrace=: which stage of an exec refused the image (the same boot-arg as hakios.systrace.c's) */
extern uint32_t hakios_exec_trace;
void hakios_exec_trace_report(struct image_params *imgp, const char *stage, int error);
void hakios_exec_trace_signature(proc_t p, struct image_params *imgp, int error, int cs_error);

#define HAKIOS_EXEC_TRACE(imgp, stage, error) \
	do { \
		if (__improbable((error) != 0 && hakios_exec_trace != 0)) { \
			hakios_exec_trace_report((imgp), (stage), (error)); \
		} \
	} while (0)

#define HAKIOS_EXEC_TRACE_SIGNATURE(p, imgp, error, cs_error) \
	do { \
		if (__improbable((error) != 0 && hakios_exec_trace != 0)) { \
			hakios_exec_trace_signature((p), (imgp), (error), (cs_error)); \
		} \
	} while (0)

const char *hakios_exec_applog_path(int fd, const char *path);
const char *hakios_exec_extra_env(struct image_params *imgp, unsigned *cursor, char *buf, size_t size);
bool hakios_exec_bumps_pidversion(proc_t p);
int hakios_spawn_adopt_credential(proc_t p, kauth_cred_t cred);

#endif /* _SYS_HAKIOS_EXEC_H_ */
