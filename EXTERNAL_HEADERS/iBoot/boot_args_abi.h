#ifndef _IBOOT_BOOT_ARGS_ABI_H_
#define _IBOOT_BOOT_ARGS_ABI_H_

/*
 * Minimal stub of the iBoot boot arguments ABI definitions needed by the
 * open-source XNU kernel build. Only the maximum environment variable data
 * size constant is required for sizing the command line buffer.
 */
#define IBOOT_MAX_ENV_VAR_DATA_SIZE 4096

#endif /* _IBOOT_BOOT_ARGS_ABI_H_ */
