/*
 * Starbite: which form the kernel serves mach_ports_register / mach_ports_lookup (task.defs ids 3403, 3404) in.
 * 0: this kernel's own (three ports inline: _kernelrpc_mach_ports_register3 / _lookup3), what iOS 26 and macOS 26
 *    user space send. 1: the older out-of-line port array, what macOS 13 (Ventura) user space sends; its launchd
 *    could not register its inheritance port and every process's libxpc died in mach_ports_lookup with
 *    MACH_RCV_TOO_LARGE (boots 2026-11-04 23:24-23:49). The Ventura kernel is built with 1
 *    (tools/build-xnu-12377.sh with SWEET_LEGACY_PORTS=1), every other one with 0.
 */
#ifndef SWEET_LEGACY_PORT_ARRAYS
#define SWEET_LEGACY_PORT_ARRAYS 0
#endif
