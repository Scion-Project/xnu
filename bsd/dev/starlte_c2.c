/*
 * kern.hakios.c2: the counters of core power-down in idle (osfmk/arm64/sweet_c2.c, boot-arguments sweetc2 and
 * sweetc2dry; scion-kernel docs/ios27/CPU-IDLE.md). NOT run on a phone.
 *
 *   kern.hakios.c2.stats   (read) the same lines the "STARLTE C2:" console line prints: per core, power-downs, how
 *                          each ended (back through the reset vector, PSCI returned, PSCI failed), time asleep, core 0's
 *                          wake-ups, refusals by reason; with sweetc2dry the predicted and measured idle histograms
 *   kern.hakios.c2.enable  (read/write, root) 1: an armed c2 runs; 0: every core stays in WFI. It cannot arm c2 that
 *                          the boot-arguments and the self-check did not.
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <machine/atomic.h>
#include <kern/kalloc.h>

extern size_t sweet_c2_format(char *buf, size_t len);
extern uint32_t sweet_c2_enable;

SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, c2, CTLFLAG_RW | CTLFLAG_LOCKED, 0,
    "core power-down in idle (osfmk/arm64/sweet_c2.c, sweetc2=)");

static int
starlte_c2_stats(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	const size_t len = 2048;
	char *buf = kalloc_data(len, Z_WAITOK | Z_ZERO);
	if (buf == NULL) {
		return ENOMEM;
	}
	size_t n = sweet_c2_format(buf, len - 1);
	buf[n] = 0;
	int err = SYSCTL_OUT(req, buf, n + 1);
	kfree_data(buf, len);
	return err;
}
SYSCTL_PROC(_kern_hakios_c2, OID_AUTO, stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    starlte_c2_stats, "A", "per-core counters of core power-down in idle");

static int
starlte_c2_enable(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int v = (int)os_atomic_load(&sweet_c2_enable, relaxed);
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err != 0 || req->newptr == USER_ADDR_NULL) {
		return err;
	}
	os_atomic_store(&sweet_c2_enable, v != 0 ? 1u : 0u, relaxed);
	return 0;
}
SYSCTL_PROC(_kern_hakios_c2, OID_AUTO, enable, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    starlte_c2_enable, "I", "1: an armed c2 runs; 0: WFI only");
#endif
