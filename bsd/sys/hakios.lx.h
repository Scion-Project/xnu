/*
 * The whole of what the Wi-Fi driver's object asks of the kernel it is linked into. That object
 * (tools/mk-ath11k.sh in the repository: Linux's ath11k on FreeBSD's LinuxKPI headers and our
 * wifi/lx) is compiled as FreeBSD kernel code, drivers/wifi/lx/hakios.lx.c on this side as XNU's;
 * neither sees the other's headers, so nothing but C's own types crosses (and the station
 * boundary's, hakios.sta.h, which is written for both). Everything the
 * driver's side knows of locks, timers, work queues and buffers is built in wifi/lx/src on the
 * few calls here.
 *
 * Threads: the driver's code runs on threads made by lx_os_thread() and on the kernel's own
 * threads that call into it; never in an interrupt.
 */
#ifndef _SYS_HAKIOS_LX_H_
#define _SYS_HAKIOS_LX_H_

#include "hakios.sta.h"

/* ---- memory ---- */
void	*lx_os_alloc(unsigned long size);			/* zeroed; NULL when there is none */
void	 lx_os_free(void *memory, unsigned long size);
/*
 * Memory the radio itself reads and writes: whole pages, zeroed, which the radio knows from
 * *address on (the machine says where: the radio sees memory through an SMMU). What either
 * side writes the other sees at once: the processor's cache is not in between, which makes
 * reading it slow. NULL when there is none; never given back.
 */
void	*lx_os_radio_memory(unsigned long size, unsigned long long *address);
/* true: the radio's reads and writes look in the processors' caches (the S9 with wifi_pcie_coherent=1), and its memory is
 * then the kernel's own, cached; false (the default, weak, in hakios.lx.c): the uncached view above */
bool	 hakios_wifi_radio_coherent(void);
void	*lx_os_map_registers(unsigned long long physical, unsigned long size);	/* never unmapped */

/* ---- saying things ---- */
void	 lx_os_print(const char *text);				/* for whoever looks for a fault: may be kept back */
void	 lx_os_tell(const char *text);				/* for whoever runs the machine: always said */
/* the driver's own account of itself, for the USB console's "wifi" (hakios_wifi_report()); NULL: no longer there */
void	 lx_os_wifi_state(void (*say)(void));
/* what is said between lx_os_failure(true) and (false) (the driver's state when its setup failed) is kept apart and
 * said again with the rest of the radio's log, on the USB console's "wifi" and when the console is first up */
void	 lx_os_failure(bool on);
void	 lx_os_panic(const char *text) __attribute__((noreturn));

/* ---- time, threads, sleeping ---- */
unsigned long long lx_os_nanoseconds(void);			/* since some moment; never goes back */
void	*lx_os_thread_self(void);				/* a value that tells the kernel's threads apart */
/* a thread that runs function(argument) and ends when that returns: 0, or an errno */
int	 lx_os_thread(const char *name, void (*function)(void *), void *argument);
/*
 * One lock, and sleeping under it as with a condition variable: lx_os_sleep() is called with the
 * lock held, gives it up while asleep and has it again when it returns -- 0 when woken by
 * lx_os_wakeup() on the same address, 1 when the time (nanoseconds; 0: no limit) ran out.
 * The lock is held for a few instructions at a time and never across a call out of the layer.
 */
void	 lx_os_lock(void);
void	 lx_os_unlock(void);
int	 lx_os_sleep(const void *address, unsigned long long nanoseconds);
void	 lx_os_wakeup(const void *address);			/* everyone asleep on it */

/* ---- the rest of the machine ---- */
void	 lx_os_random(void *buffer, unsigned long size);	/* bytes nobody can guess */
/* a number given to the kernel when it was started ("wifi_lx=2"); 0 if it was not */
unsigned lx_os_option(const char *name);
/* a file the driver asks for by the name Linux has for it, in place: 0, or an errno (ENOENT) */
int	 lx_os_firmware(const char *name, const void **data, unsigned long *size);
/* the processor that runs the radio's firmware: started, and up when this returns; 0 or an errno */
int	 lx_os_rproc_boot(void);
/* the radio's own address (six bytes), if the machine has one for it: 1, or 0 and nothing written */
int	 lx_os_radio_address(unsigned char *address);
/* the BCM4361's CIS from its OTP (length bytes, chip ID, revision): which module it is, for the files it is given */
void	 lx_os_radio_identity(const unsigned char *cis, unsigned long length, unsigned chip, unsigned revision);
/*
 * The network the radio is to join, if the machine was given one: its name and its passphrase,
 * into buffers whose sizes come in and whose contents' lengths go out. 1, or 0.
 */
int	 lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length);

/* ---- frames: the radio as what carries the machine's network interface ---- */
void	 lx_os_net_attach(const unsigned char *address);	/* there is a radio, and this is its address */
void	 lx_os_net_link(int up);				/* frames can pass now (a network is joined) / no longer */
void	 lx_os_net_input(const unsigned char *frame, unsigned long length);	/* an Ethernet frame has arrived */

/*
 * ---- the radio's interrupts (wifi_irq=1). The radio raises one by writing a number to an address
 * (an "MSI": the GIC's register that raises the SPI of that number); vector v of its 32 is data + v.
 * The driver's work for one is done in thread context, on the kernel's thread for them: the
 * function given for a vector is called there after it has fired, with the vector masked until
 * lx_os_irq_enable(). Nothing of the driver runs in an interrupt.
 */
int	 lx_os_irq_msi(unsigned long long *address, unsigned *data);	/* where and what: 1, or 0 (no interrupts) */
int	 lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument);	/* masked to begin with; 0 or an errno */
void	 lx_os_irq_enable(unsigned vector);
void	 lx_os_irq_disable(unsigned vector);			/* returns when its function is not running */
void	 lx_os_irq_detach_all(void);				/* returns when no function is running */
unsigned long long lx_os_irq_count(unsigned vector);		/* how many have arrived */

/*
 * ---- or one line, level-triggered, which the radio's driver shares with nobody (the Galaxy S9's INTx, SPI 253, for
 * brcmfmac; the WCN6750's kernel has none of these, and ath11k's object does not ask for them). The same contract as a
 * vector's: function(argument) is called on the kernel's thread for it after the line has fired, with the line masked and
 * the root complex's latch of it already cleared, until lx_os_irq_line_enable(); attached masked. 0, or an errno.
 */
int	 lx_os_irq_line_attach(void (*function)(void *), void *argument);
void	 lx_os_irq_line_enable(void);
void	 lx_os_irq_line_detach(void);				/* returns when function is not running (unless it is the caller) */
unsigned long long lx_os_irq_line_count(void);			/* how many have arrived */

/*
 * ---- a radio on PCIe (the Galaxy S9's BCM4361, for brcmfmac: drivers/wifi/brcmfmac/hakios.brcmfmac.c; the WCN6750's kernel has
 * none of these, and ath11k's object does not ask for them). One device, already enumerated and decoding.
 */
int	 lx_os_pci_device(unsigned *vendor, unsigned *device);	/* 1 if there is one, with its IDs; 0 */
unsigned lx_os_pci_config_read(unsigned offset);		/* a 32-bit word of its configuration space (offset & ~3) */
void	 lx_os_pci_config_write(unsigned offset, unsigned value);
/* where BAR n is (the processor's physical address, which is also the bus's) and how big: 1, or 0 if it has none */
int	 lx_os_pci_bar(unsigned bar, unsigned long long *address, unsigned long long *size);
/* what stands between the radio and memory (the SysMMU, the shareability, both ends' status), said and kept */
void	 lx_os_pci_say_dma(const char *when);

/* ---- the station: scanning and joining, for whoever asks the kernel (hakios.sta.h) ---- */
void	 lx_os_station(const struct hakios_sta_ops *ops);	/* here it is, to be asked; NULL: no longer (returns when no call is under way) */
void	 lx_os_station_lost(int error);				/* the network joined is gone by itself */

/*
 * ---- the names the object shows the kernel. lx_start(): bring the driver up on the radio and
 * do what the kernel's options ask (scan; join a network). 0, or an errno. Returns when that is
 * over or has failed; with a network joined the driver stays up, lx_transmit() sends an
 * Ethernet frame through it (0, or an errno; called by one thread, the kernel's own for that),
 * and lx_stop() leaves the network and takes the driver down in order (and does nothing if it
 * is not up).
 */
int	 lx_start(void);
void	 lx_stop(void);
int	 lx_transmit(const unsigned char *frame, unsigned long length);
/* before lx_start(), if the kernel wants it early: take the memory the radio will be shown. 0, or an errno. */
int	 lx_prepare(void);

#endif
