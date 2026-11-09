/*
 * A driver's log line: to the kernel's log, and the loader's, which survives the run. At most 600 bytes.
 */
#ifndef _SYS_HAKIOS_LOG_H_
#define _SYS_HAKIOS_LOG_H_

#include <sys/cdefs.h>

int hakios_say(const char *format, ...) __printflike(1, 2);
#define say hakios_say

#endif /* _SYS_HAKIOS_LOG_H_ */
