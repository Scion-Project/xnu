/*
 * Copyright (c) 2000-2007 Apple Inc. All rights reserved.
 */
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>

#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* osfmk/arm/arm_init.c: the loader's line with the S9's built-in defaults after it (PE_BOOT_ARGS_LENGTH bytes). */
extern char starlte_boot_line[];
#endif

char *
PE_boot_args(
	void)
{
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (starlte_boot_line[0] != '\0') {
		return starlte_boot_line;
	}
#endif
	return (char *)((boot_args *)PE_state.bootArgs)->CommandLine;
}
