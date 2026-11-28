/*
 * Built-in AMFI interface for SWEET, A5XELTE and HEROLTE.
 *
 * On Apple platforms AppleMobileFileIntegrity registers this interface and
 * provides entitlements through the closed CoreEntitlements library. SWEET
 * has neither, and code signing reaches it on every exec (ubc_cs_blob_add).
 *
 * Entitlements: when a code signature is added, its XML entitlements blob
 * (already checked against the code directory) is parsed into a
 * SweetEntitlements object (iokit/Kernel/arm/SweetEntitlements.cpp), set as
 * the blob's csb_entitlements, and every query is answered from it. DER-only
 * signatures get none. Code signature and page hash validation stay with the
 * kernel. Trust caches remain unavailable (no Image4), so the TrustCache
 * interface is left at version 0.
 *
 * Platform binaries: AMFI marks binaries whose cdhash is in a trust cache
 * as platform binaries from its vnode_check_signature MAC hook. Without
 * trust caches, the SWEET policy below marks every binary on the root
 * filesystem instead (the iOS system binaries are ad-hoc signed and are
 * platform binaries only through the static trust cache).
 *
 * Interfaces no kernel path uses on SWEET (they serve other Apple kexts)
 * panic with a clear message instead of being NULL.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <machine/atomic.h>
#include <kern/startup.h>
#include <libkern/amfi/amfi.h>
#include <mach/kern_return.h>
#include <pexpert/pexpert.h>
#include <sys/codesign.h>
#include <sys/mount.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/proc_private.h>
#include <kern/task.h>
#include <vm/vm_map_xnu.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/ubc_internal.h>
#include <sys/vnode.h>
#include <security/mac_policy.h>

/* iokit/Kernel/arm/SweetEntitlements.cpp */
void *sweet_entitlements_create(const CS_GenericBlob *blob, size_t blob_len);
void *sweet_entitlements_copy_dict(const void *ent);
bool sweet_entitlements_copy_xml(const void *ent, CS_GenericBlob **out);
bool sweet_entitlements_bool(const void *ent, const char *name);
bool sweet_entitlements_string(const void *ent, const char *name, const char *value);
void *sweet_entitlements_copy_object(const void *ent, const char *name);
unsigned int sweet_entitlements_count(const void *ent);
extern void osobject_release(void *object);

/* boot-arg sweet_amfi_debug=1: log the entitlements attached to each signature. */
static TUNABLE(uint32_t, sweet_amfi_debug, "sweet_amfi_debug", 0);

/* The entitlements object of a process's main binary, or NULL. */
static void *
sweet_proc_entitlements(proc_t proc)
{
	struct cs_blob *blob = csproc_get_blob(proc);
	return blob != NULL ? csblob_os_entitlements_get(blob) : NULL;
}

static void
sweet_amfi_entitlements_invalidate(__unused void *os_entitlements)
{
	/* Queries already require a valid signature (csblob_get_entitlements). */
}

static void *
sweet_amfi_entitlements_as_dict(void *os_entitlements)
{
	return sweet_entitlements_copy_dict(os_entitlements);
}

static CEError_t
sweet_amfi_entitlements_query(__unused void *os_entitlements,
    __unused uint8_t cdhash[CS_CDHASH_LEN], __unused CEQuery_t query,
    __unused size_t query_length)
{
	panic("SWEET AMFI: CoreEntitlements queries are not implemented");
}

static bool
sweet_amfi_entitlements_get_transmuted(__unused void *os_entitlements,
    __unused const CS_GenericBlob **blob)
{
	return false;
}

static bool
sweet_amfi_entitlements_get_xml(void *os_entitlements, CS_GenericBlob **blob)
{
	return sweet_entitlements_copy_xml(os_entitlements, blob);
}

static bool
sweet_amfi_get_legacy_profile_exemptions(__unused const uint8_t **profile,
    __unused size_t *profile_length)
{
	return false;
}

static bool
sweet_amfi_get_udid(__unused const uint8_t **udid, __unused size_t *udid_length)
{
	return false;
}

static void *
sweet_amfi_query_context_to_object(__unused CEQueryContext_t ctx)
{
	panic("SWEET AMFI: CoreEntitlements query contexts are not implemented");
}

static kern_return_t
sweet_amfi_adjust_with_monitor(__unused void *os_entitlements,
    __unused const CEQueryContext_t ce_ctx, __unused const void *monitor_sig_obj,
    __unused const char *identity, __unused const uint32_t code_signing_flags)
{
	/* SWEET has no code signing monitor. */
	return KERN_NOT_SUPPORTED;
}

/*
 * Attach the parsed XML entitlements to a code signature being added. This
 * must happen in the vnode_check_signature MAC hook, like AMFI does: later,
 * converting a 4K-page signature to multilevel hashes drops the XML blob.
 * Nothing is granted when the blob is missing, unreadable or does not match
 * its code directory hash.
 */
static void
sweet_amfi_attach_entitlements(struct cs_blob *code_signing_blob)
{
	void *xml = NULL;
	size_t xml_len = 0;

	if (csblob_get_entitlements(code_signing_blob, &xml, &xml_len) != 0) {
		printf("SWEET AMFI: %s: entitlements do not match the code directory, granting none\n",
		    csblob_get_identity(code_signing_blob));
		return;
	}
	if (xml == NULL) {
		return;                 /* no XML entitlements */
	}
	void *ent = sweet_entitlements_create(xml, xml_len);
	if (ent == NULL) {
		printf("SWEET AMFI: %s: unreadable entitlements (%zu bytes), granting none\n",
		    csblob_get_identity(code_signing_blob), xml_len);
		return;
	}
	if (sweet_amfi_debug) {
		/* The test keys only exist in tools/mkinit.py test signatures. */
		printf("SWEET AMFI: %s: %u entitlements (test: bool %d string %d array %d)\n",
		    csblob_get_identity(code_signing_blob), sweet_entitlements_count(ent),
		    sweet_entitlements_bool(ent, "com.apple.private.sweet.test-bool"),
		    sweet_entitlements_string(ent, "com.apple.private.sweet.test-string", "hello"),
		    sweet_entitlements_string(ent, "com.apple.private.sweet.test-array", "two"));
	}
	csblob_os_entitlements_set(code_signing_blob, ent);    /* takes a reference */
	osobject_release(ent);
}

static kern_return_t
sweet_amfi_adjust_without_monitor(__unused void *os_entitlements,
    __unused struct cs_blob *code_signing_blob)
{
	/* Entitlements were attached in sweet_amfi_vnode_check_signature. */
	return KERN_SUCCESS;
}

static kern_return_t
sweet_amfi_query_boolean(const void *os_entitlements, const char *entitlement_name)
{
	return sweet_entitlements_bool(os_entitlements, entitlement_name) ? KERN_SUCCESS : KERN_DENIED;
}

static kern_return_t
sweet_amfi_query_boolean_with_proc(const proc_t proc, const char *entitlement_name)
{
	return sweet_amfi_query_boolean(sweet_proc_entitlements(proc), entitlement_name);
}

static kern_return_t
sweet_amfi_query_string(const void *os_entitlements,
    const char *entitlement_name, const char *entitlement_value)
{
	return sweet_entitlements_string(os_entitlements, entitlement_name, entitlement_value) ?
	       KERN_SUCCESS : KERN_DENIED;
}

static kern_return_t
sweet_amfi_query_string_with_proc(const proc_t proc,
    const char *entitlement_name, const char *entitlement_value)
{
	return sweet_amfi_query_string(sweet_proc_entitlements(proc), entitlement_name,
	           entitlement_value);
}

static kern_return_t
sweet_amfi_copy_object(const void *os_entitlements,
    const char *entitlement_name, void **entitlement_object)
{
	void *obj = sweet_entitlements_copy_object(os_entitlements, entitlement_name);
	if (entitlement_object) {
		*entitlement_object = obj;
	} else if (obj) {
		osobject_release(obj);
	}
	return obj ? KERN_SUCCESS : KERN_NOT_FOUND;
}

static kern_return_t
sweet_amfi_copy_object_with_proc(const proc_t proc,
    const char *entitlement_name, void **entitlement_object)
{
	return sweet_amfi_copy_object(sweet_proc_entitlements(proc), entitlement_name,
	           entitlement_object);
}

static const amfi_t sweet_amfi = {
	.OSEntitlements_invalidate = sweet_amfi_entitlements_invalidate,
	.OSEntitlements_asdict = sweet_amfi_entitlements_as_dict,
	.OSEntitlements_query = sweet_amfi_entitlements_query,
	.OSEntitlements_get_transmuted = sweet_amfi_entitlements_get_transmuted,
	.OSEntitlements_get_xml = sweet_amfi_entitlements_get_xml,
	/* CoreEntitlements is only used by the PMAP_CS code signing monitor. */
	.get_legacy_profile_exemptions = sweet_amfi_get_legacy_profile_exemptions,
	.get_udid = sweet_amfi_get_udid,
	.query_context_to_object = sweet_amfi_query_context_to_object,
	/* .TrustCache stays version 0: trust caches are unavailable without Image4. */
	.OSEntitlements = {
		.version = OSENTITLEMENTS_INTERFACE_VERSION,
		.adjustContextWithMonitor = sweet_amfi_adjust_with_monitor,
		.adjustContextWithoutMonitor = sweet_amfi_adjust_without_monitor,
		.queryEntitlementBoolean = sweet_amfi_query_boolean,
		.queryEntitlementBooleanWithProc = sweet_amfi_query_boolean_with_proc,
		.queryEntitlementString = sweet_amfi_query_string,
		.queryEntitlementStringWithProc = sweet_amfi_query_string_with_proc,
		.copyEntitlementAsOSObject = sweet_amfi_copy_object,
		.copyEntitlementAsOSObjectWithProc = sweet_amfi_copy_object_with_proc,
	},
};

__startup_func
static void
sweet_amfi_register(void)
{
	amfi_interface_register(&sweet_amfi);
	printf("SWEET AMFI: built-in interface registered (XML entitlements)\n");
}
STARTUP(EARLY_BOOT, STARTUP_RANK_MIDDLE, sweet_amfi_register);

/*
 * The signature reached this hook validated, so mark it CS_SIGNED like AMFI
 * does: exec copies the flag to the process, and csproc_get_blob() (csops
 * status, cdhash, platform and entitlement queries by process) ignores
 * processes without it. Every signed binary on the root filesystem is a
 * platform binary. New signatures (ubc_cs_blob_add passes its writable copy;
 * revalidation passes the read-only blob itself) get their entitlements here.
 */
static int
sweet_amfi_vnode_check_signature(struct vnode *vp, __unused struct label *label,
    __unused cpu_type_t cpu_type, struct cs_blob *cs_blob,
    unsigned int *cs_flags, __unused unsigned int *signer_type,
    __unused int flags, __unused unsigned int platform,
    __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	mount_t mp = vnode_mount(vp);
	*cs_flags |= CS_SIGNED;
	if (mp != NULL && (vfs_flags(mp) & MNT_ROOTFS)) {
		*cs_flags |= CS_PLATFORM_BINARY;
	}
	if (cs_blob != cs_blob->csb_ro_addr && cs_blob->csb_entitlements == NULL) {
		sweet_amfi_attach_entitlements(cs_blob);
	}
	return 0;
}

/*
 * Policy "Sandbox": libsystem_sandbox calls __mac_syscall("Sandbox", op,
 * arg). Without a policy of that name every call fails with ENOPOLICY and
 * launchd reports mach lookups as "error = 159: Sandbox restriction"
 * (backboardd cannot reach mobilegestalt or iohideventsystem; tccd:
 * "profile not found"). SWEET runs without a sandbox: operations succeed
 * (checks allow, profile init is accepted); the few that must return
 * data answer what is known (container paths: below; extension tokens:
 * sweet_sbx_ext) and fail with ENOENT otherwise. The first
 * use of each operation number is logged.
 */
static uint64_t sweet_sandbox_seen;

/*
 * Sandbox extensions (on by default on STARLTE: sweet_sbx_ext=0 turns them off; elsewhere boot-arg sweetspeed&0x2000000,
 * which every board still honours). A process hands another one
 * the right to a file as a token: sandbox_extension_issue_* (operation 5) makes it,
 * sandbox_extension_consume (6) turns it into a handle, release (7) gives the handle back. With no
 * sandbox every process already has every right, so a token grants nothing, but the programs check
 * that they got one: PosterKit logs "failed to issue sandbox extension to PID ..." for every poster
 * path it sends to a poster extension. With the bit, issue answers a token (the class and the path,
 * readable, behind a fixed prefix) and consume answers a handle (a counter); nothing is recorded
 * and nothing is checked. Argument layouts as libsystem_sandbox of iOS 27.0.1 passes them:
 *   issue:   { class, type, data, flags, token buffer (0x3ff0 bytes), pid, flags2 }, 8 bytes each
 *   consume: { token, its length with the NUL, address of an int64 handle }
 */
#define SWEET_SPEED_SBX_EXT     0x2000000
#define SWEET_SBX_TOKEN_MAX     1280
extern uint32_t sweet_speed_flags(void);        /* boot-arg sweetspeed (osfmk/arm64/sweet_a64e.c) */

/*
 * On by default on STARLTE (17 November 2026): tccd hands the token with every answer it gives, and without one it turns
 * an "Allow" into an error ("Failed to issue Sandbox Extension for kTCCServiceCamera", Camera.app's -11852), so the camera,
 * the microphone and the photo library's access by TCC depended on a bit of a test line. A token grants nothing here (no
 * sandbox is enforced: every process already has every right it names); what changes is that the programs that ask for
 * one go on as on an iPhone instead of taking their error path. TCC's own decisions are unchanged.
 */
#if defined(ARM64_BOARD_CONFIG_STARLTE)
#define SWEET_SBX_EXT_DEFAULT   1
#else
#define SWEET_SBX_EXT_DEFAULT   0
#endif
static TUNABLE(uint32_t, sweet_sbx_ext, "sweet_sbx_ext", SWEET_SBX_EXT_DEFAULT);
static _Atomic int64_t sweet_sandbox_handle;

static int
sweet_sandbox_extension_issue(user_addr_t arg)
{
	struct { uint64_t cls, type, data, flags, token, pid, flags2; } a;
	char cls[128] = "", data[1024] = "";
	char *token;
	size_t done;
	int error;

	if ((error = copyin(arg, &a, sizeof(a))) != 0) {
		return error;
	}
	if (a.token == 0) {
		return EINVAL;
	}
	if (a.cls != 0 && copyinstr((user_addr_t)a.cls, cls, sizeof(cls), &done) != 0) {
		cls[0] = 0;
	}
	if (a.data != 0 && copyinstr((user_addr_t)a.data, data, sizeof(data), &done) != 0) {
		data[0] = 0;
	}
	token = kalloc_data(SWEET_SBX_TOKEN_MAX, Z_WAITOK | Z_ZERO);
	if (token == NULL) {
		return ENOMEM;
	}
	snprintf(token, SWEET_SBX_TOKEN_MAX, "scion-no-sandbox;%llu;%s;%s", (unsigned long long)a.type, cls, data);
	error = copyout(token, (user_addr_t)a.token, strlen(token) + 1);
	kfree_data(token, SWEET_SBX_TOKEN_MAX);
	return error;
}

static int
sweet_sandbox_extension_consume(user_addr_t arg)
{
	struct { uint64_t token, length, handle; } a;
	int64_t handle;
	int error;

	if ((error = copyin(arg, &a, sizeof(a))) != 0) {
		return error;
	}
	if (a.handle == 0) {
		return EINVAL;
	}
	handle = os_atomic_inc(&sweet_sandbox_handle, relaxed);
	return copyout(&handle, (user_addr_t)a.handle, sizeof(handle));
}

/*
 * Operation 4, "container path of a process": sandbox_container_path_for_pid() and
 * sandbox_container_path_for_audit_token() of libsystem_sandbox (iOS 27: { pid, its version, buffer, size, whether the
 * version is to be compared }, 8 bytes each). It used to fail with ENOENT for everyone, and an application extension
 * hosted by PlugInKit does not run without an answer: -[PKService checkEnvironment:] puts "?" in a 1 KiB buffer, asks
 * for its own container and raises NSInternalInconsistencyException (with errno) unless the call succeeds with a path
 * longer than one byte. Every PlugInKit extension died there at start: SettingsImportExtension and the Spotlight index
 * extensions of Music, TV, Books, Home, Tips, Notes, Phone, Podcasts, Voice Memos, SupportFlow and Contacts, 30 to 70
 * aborts a boot (24B156), each one relaunched by its host.
 *
 * On an iPhone the Sandbox learns the container when the program is executed. Here nothing is confined, but whoever
 * starts a program that lives in a container (RunningBoard an application, pkd an extension) has had its path from
 * containermanagerd and passes it in the environment as CFFIXED_USER_HOME, where Foundation takes the home directory
 * from. That is the answer: it is looked up among the strings exec left at the top of the process's stack (as
 * sysctl kern.procargs2 reads them). A process without the variable has no container and gets ENOENT, as before.
 */
#define SWEET_SBX_CONTAINER_VARIABLE    "CFFIXED_USER_HOME="

static int
sweet_sandbox_container_path(user_addr_t arg)
{
	struct {
		int64_t     pid;
		uint64_t    pidversion;
		user_addr_t buffer;
		user_size_t size;
		uint64_t    check_pidversion;
	} a;
	const size_t prefix = sizeof(SWEET_SBX_CONTAINER_VARIABLE) - 1;
	user_addr_t user_stack;
	size_t argslen, strings_size, len;
	const char *s, *end;
	char *strings;
	vm_map_t map;
	task_t task;
	proc_t p;
	int error;

	if ((error = copyin(arg, &a, sizeof(a))) != 0) {
		return error;
	}
	if (a.buffer == 0 || a.size == 0) {
		return EINVAL;
	}
	p = proc_find((int)a.pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}
	if (a.check_pidversion && (uint64_t)proc_pidversion(p) != a.pidversion) {
		proc_rele(p);
		return ESRCH;
	}
	user_stack = p->user_stack;
	argslen = p->p_argslen;
	task = proc_task(p);
	if (user_stack == 0 || argslen == 0 || argslen > ARG_MAX || task == TASK_NULL) {
		proc_rele(p);
		return ENOENT;
	}
	/* as sysctl_procargsx(): the task keeps the map alive once the process is let go */
	task_reference(task);
	proc_rele(p);
	map = get_task_map_reference(task);
	task_deallocate(task);
	if (map == VM_MAP_NULL) {
		return ENOENT;
	}
	strings_size = vm_map_round_page(argslen, vm_map_page_mask(map));
	strings = kalloc_data(strings_size, Z_WAITOK | Z_ZERO);
	if (strings == NULL) {
		vm_map_deallocate(map);
		return ENOMEM;
	}
	if (vm_map_read_user(map, (vm_map_address_t)(user_stack - strings_size), strings, strings_size) != KERN_SUCCESS) {
		vm_map_deallocate(map);
		kfree_data(strings, strings_size);
		return ENOENT;          /* the process is on its way out, or has unmapped its own stack */
	}
	vm_map_deallocate(map);

	error = ENOENT;
	end = strings + strings_size;
	for (s = end - argslen; s < end; s += len + 1) {
		len = strnlen(s, end - s);
		if (len > prefix && s + len < end && strncmp(s, SWEET_SBX_CONTAINER_VARIABLE, prefix) == 0) {
			s += prefix;
			len -= prefix;
			error = len < a.size ? copyout(s, a.buffer, len + 1) : ENAMETOOLONG;
			break;
		}
	}
	kfree_data(strings, strings_size);
	return error;
}

static int
sweet_sandbox_syscall(struct proc *p, int call, user_addr_t arg)
{
	if (call >= 0 && call < 64 && !(sweet_sandbox_seen & (1ULL << call))) {
		sweet_sandbox_seen |= 1ULL << call;
		char name[32];
		proc_name(proc_pid(p), name, sizeof(name));
		printf("SWEET Sandbox: %s op %d\n", name, call);
	}
	if ((call == 5 || call == 6) && (sweet_sbx_ext != 0 || (sweet_speed_flags() & SWEET_SPEED_SBX_EXT) != 0)) {
		return call == 5 ? sweet_sandbox_extension_issue(arg) : sweet_sandbox_extension_consume(arg);
	}
	switch (call) {
	case 4:         /* container path (audit token / pid) */
		return sweet_sandbox_container_path(arg);
	case 5:         /* extension issue */
	case 6:         /* extension consume */
		return ENOENT;
	case 0x43:
		/* container expected: libsystem_sandbox passes two out-pointers
		 * it has zeroed and aborts _container_init() on any error (iOS 18:
		 * "sandbox_get_container_expected() failed: -1", launchd).
		 * Success without writing = no container expected. */
		return 0;
	default:
		return 0;
	}
}

/*
 * __mac_syscall("AMFI", op, arg). Op 90 = amfi_check_dyld_policy_self():
 * arg -> { uint64_t input_flags; user_addr_t output_flags }. Without a policy
 * named "AMFI" every process got ENOPOLICY, so libsystem_trace treated the
 * device as non-development and ignored ACTIVITY_LOG_STDERR / OS_ACTIVITY_*
 * (needed to read daemons' os_log on the console). Answer bit 8
 * (AMFI_DYLD_OUTPUT_ALLOW_DEVELOPMENT_VARS); with boot-arg sweetinject
 * (kern_exec.c inserts libraries) also path variables (1 << 1, for
 * DYLD_INSERT_LIBRARIES), print variables (1 << 4), failed insertion
 * (1 << 5: a missing library does not kill the process) and interposing
 * (1 << 6). Other ops keep answering ENOPOLICY.
 */
static uint64_t sweet_amfi_seen[2];

static int
sweet_amfi_syscall(struct proc *p, int call, user_addr_t arg)
{
	if (call >= 0 && call < 128 && !(sweet_amfi_seen[call / 64] & (1ULL << (call % 64)))) {
		char name[32];
		sweet_amfi_seen[call / 64] |= 1ULL << (call % 64);
		proc_name(proc_pid(p), name, sizeof(name));
		printf("SWEET AMFI: %s op %d\n", name, call);
	}
	if (call == 90) {
		struct { uint64_t input; uint64_t output; } a;
		uint64_t flags = 1ULL << 8;
		if (PE_parse_boot_argn("sweetinject", NULL, 0)) {
			flags |= (1ULL << 1) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6);
		}
		/*
		 * sweetatpath=1: also bit 0 (AMFI_DYLD_OUTPUT_ALLOW_AT_PATH), as AMFI answers an ordinary signed process
		 * on a stock phone. Without it dyld refuses every @rpath / @executable_path / @loader_path ("security
		 * policy does not allow @ path expansion"): app extensions and apps that carry their own frameworks
		 * (Books' and Podcasts' widgets died in dyld at every boot). Off by default.
		 */
		uint32_t atpath = 0;
		if (PE_parse_boot_argn("sweetatpath", &atpath, sizeof(atpath)) && atpath != 0) {
			flags |= 1ULL << 0;
		}
		if (copyin(arg, &a, sizeof(a)) != 0) {
			return EFAULT;
		}
		return copyout(&flags, (user_addr_t)a.output, sizeof(flags));
	}
	if (call == 0x60) {
		/*
		 * amfi_interface_query_bootarg_state(uint64_t *state) (iOS 27 libxpc,
		 * AppleMobileFileIntegrity.framework): arg = user pointer to a
		 * uint64_t. iOS 27 ExtensionFoundation (-[_EXDefaults
		 * enforceXPCCacheCodeSigning]) reads (state & 0x86) to decide whether
		 * to skip code-signing enforcement on the XPC extension-point cache
		 * (/System/Library/xpc/extensions.plist). We have no AMFI trust cache,
		 * so with state 0 EF rejects that cache and NONE of the framework-
		 * declared extension points register (com.apple.posterkit.provider ->
		 * wallpaper picker dead; com.apple.widgetkit-extension -> widgets dead;
		 * app/extension discovery degraded). With sweet_extkit_unsigned_cache=1
		 * report bits 0x86 so the cache loads from disk unverified, matching a
		 * genuine device booted with the corresponding AMFI boot-args (and
		 * consistent with this build's existing cs_enforcement_disable /
		 * launchd_unsecure_cache posture). OFF by default. Proven-equivalent
		 * root-side workaround: FeatureFlag ExtensionKit
		 * enforce_xpc_cache_code_signing=false.
		 */
		uint64_t state = 0;
		uint32_t on = 0;
		if (PE_parse_boot_argn("sweet_extkit_unsigned_cache", &on, sizeof(on)) && on) {
			state = 0x86;   /* bits 1,2,7: EF's codeSigningEnforcementIsDisabled test */
		}
		return copyout(&state, arg, sizeof(state));
	}
	if (call == 0x66) {
		/*
		 * amfi_launch_constraint_matches_process() (iOS 26): does the process
		 * of an audit token satisfy a lightweight code requirement? libxpc
		 * asserts that the answer and result.error_code agree (backboardd
		 * aborted on ENOPOLICY with a zeroed result). Permissive like the rest:
		 * error_code 0 = AICMR_MATCH.
		 */
		struct {
			uint8_t  token[32];
			uint64_t lwcr;
			uint32_t lwcr_size;
			uint32_t pad;
			uint64_t result;
			uint32_t result_size;
		} a;
		uint32_t match = 0;
		if (copyin(arg, &a, sizeof(a)) != 0) {
			return EFAULT;
		}
		if (a.result == 0 || a.result_size < sizeof(match)) {
			return EINVAL;
		}
		return copyout(&match, (user_addr_t)a.result, sizeof(match));
	}
	return ENOPOLICY;
}

static const struct mac_policy_ops sweet_amfi_mac_ops = {
	.mpo_vnode_check_signature = sweet_amfi_vnode_check_signature,
	.mpo_policy_syscall = sweet_amfi_syscall,
};

/*
 * Privileges for non-root processes come from MAC policies (priv_check_cred
 * -> mac_priv_grant); on iOS Sandbox/AMFI grant them. Without a grant UIKit
 * apps die in CoreAnimation setup: work_interval_create(CA_CLIENT) needs
 * PRIV_WORK_INTERVAL ("Failed to create work interval", SpringBoard).
 * Grant that one; log the first denial of each other privilege per process.
 */
#define SWEET_PRIV_LOG_MAX      64
static struct {
	int  priv;
	char name[20];
} sweet_priv_logged[SWEET_PRIV_LOG_MAX];
static int sweet_priv_logged_count;

/*
 * Privileges iOS grants by an entitlement of the process, with sweet_priv_ent=1 (on by default on STARLTE since 28 November 2026, in the kernel's built-in line; 0 here elsewhere): cameracaptured has both
 * (iOS 27.0.1's launchd job binary). PRIV_SYSTEM_OVERRIDE is asked by CMCapture's capture-session prewarm,
 * system_override(..., SYS_OVERRIDE_FAST_JETSAM), whose result it does not look at; PRIV_PROC_CPUMON_OVERRIDE by the
 * process's own CPU monitor setting ("denied attempt to escape CPU monitor"). Neither stopped the camera in the phone test of
 * 17 November; granting them makes cameracaptured run as on an iPhone. On every test phone's line since 18 November
 * (terminusd's NECP abort gone: PR 60).
 */
static TUNABLE(uint32_t, sweet_priv_ent, "sweet_priv_ent", 0);
static const struct {
	int             priv;
	const char      *entitlement;
} sweet_priv_entitlements[] = {
	{ PRIV_SYSTEM_OVERRIDE, "com.apple.private.kernel.system-override" },
	{ PRIV_PROC_CPUMON_OVERRIDE, "com.apple.private.kernel.override-cpumon" },
	/*
	 * Denied on the 18 November restore boot (every iOS 27 daemon back; daemon-restore census), each asked by daemons
	 * that carry the entitlement on 24B156: terminusd aborts (_NRLogAbortWithPack, SIGTRAP) right after its NECP
	 * session open is refused (10004); networkserviceproxy, rapportd and mDNSResponder ask the same.
	 */
	{ PRIV_GLOBAL_PROC_INFO, "com.apple.private.kernel.global-proc-info" },              /* 1002: dasd */
	{ PRIV_VM_PRESSURE, "vm-pressure-level" },                                          /* 6000: dasd */
	{ PRIV_NET_PRIVILEGED_SOCKET_DELEGATE, "com.apple.private.network.socket-delegate" }, /* 10001: mDNSResponder, trustd, cloudd, dasd */
	{ PRIV_NET_PRIVILEGED_NETWORK_STATISTICS, "com.apple.private.network.statistics" },  /* 10003: symptomsd, terminusd */
	{ PRIV_NET_PRIVILEGED_NECP_POLICIES, "com.apple.private.necp.policies" },           /* 10004: terminusd, networkserviceproxy */
	{ PRIV_SKYWALK_REGISTER_USER_PIPE, "com.apple.private.skywalk.register-user-pipe" }, /* 12000: rapportd */
};

static int
sweet_sandbox_priv_grant(kauth_cred_t cred __unused, int priv)
{
	/* iOS 27 on STARLTE: SpringBoard sets kern.darkboot (PRIV_DARKBOOT) at start */
	if (priv == PRIV_WORK_INTERVAL || priv == PRIV_DARKBOOT) {
		return 0;
	}
	if (sweet_priv_ent) {
		for (size_t i = 0; i < sizeof(sweet_priv_entitlements) / sizeof(sweet_priv_entitlements[0]); i++) {
			if (sweet_priv_entitlements[i].priv == priv &&
			    sweet_amfi_query_boolean_with_proc(current_proc(), sweet_priv_entitlements[i].entitlement) == KERN_SUCCESS) {
				return 0;
			}
		}
	}
	char name[20];
	proc_selfname(name, sizeof(name));
	for (int i = 0; i < sweet_priv_logged_count; i++) {
		if (sweet_priv_logged[i].priv == priv &&
		    strncmp(sweet_priv_logged[i].name, name, sizeof(name)) == 0) {
			return EPERM;
		}
	}
	if (sweet_priv_logged_count < SWEET_PRIV_LOG_MAX) {
		sweet_priv_logged[sweet_priv_logged_count].priv = priv;
		strlcpy(sweet_priv_logged[sweet_priv_logged_count].name, name, sizeof(name));
		sweet_priv_logged_count++;
		printf("SWEET Sandbox: %s privilege %d denied\n", name, priv);
	}
	return EPERM;
}

static const struct mac_policy_ops sweet_sandbox_mac_ops = {
	.mpo_policy_syscall = sweet_sandbox_syscall,
	.mpo_priv_grant = sweet_sandbox_priv_grant,
};

static struct mac_policy_conf sweet_sandbox_mac_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "SWEET permissive sandbox stand-in",
	.mpc_ops = &sweet_sandbox_mac_ops,
};

static mac_policy_handle_t sweet_sandbox_mac_handle;

static struct mac_policy_conf sweet_amfi_mac_conf = {
	.mpc_name = "AMFI",                 /* libsystem calls __mac_syscall("AMFI", ...) */
	.mpc_fullname = "SWEET built-in Mobile File Integrity",
	.mpc_ops = &sweet_amfi_mac_ops,
};

static mac_policy_handle_t sweet_amfi_mac_handle;

/*
 * Policy "Quarantine" (macOS's Quarantine.kext): cfprefsd 26.5 calls __mac_syscall("Quarantine", 87)
 * at start and exits on ENOPOLICY, so every CFPreferences client waits forever. No quarantine
 * here: every operation succeeds without writing. First use of each operation is logged.
 */
static uint64_t sweet_quarantine_seen[2];

static int
sweet_quarantine_syscall(struct proc *p, int call, __unused user_addr_t arg)
{
	if (call >= 0 && call < 128 && !(sweet_quarantine_seen[call / 64] & (1ULL << (call % 64)))) {
		sweet_quarantine_seen[call / 64] |= 1ULL << (call % 64);
		char name[32];
		proc_name(proc_pid(p), name, sizeof(name));
		printf("SWEET Quarantine: %s op %d\n", name, call);
	}
	return 0;
}

static const struct mac_policy_ops sweet_quarantine_mac_ops = {
	.mpo_policy_syscall = sweet_quarantine_syscall,
};

static struct mac_policy_conf sweet_quarantine_mac_conf = {
	.mpc_name = "Quarantine",
	.mpc_fullname = "SWEET permissive quarantine stand-in",
	.mpc_ops = &sweet_quarantine_mac_ops,
};

static mac_policy_handle_t sweet_quarantine_mac_handle;

/*
 * iOS 27 on STARLTE: Darwin 27 launchd/libs read these sysctls that the
 * AMFI kext / platform provide on Apple hardware. Developer Mode on and
 * resolved; not a security research device (stock value).
 */
SYSCTL_DECL(_security_mac);
SYSCTL_DECL(_hw_features);
SYSCTL_NODE(_security_mac, OID_AUTO, amfi, CTLFLAG_RW | CTLFLAG_LOCKED, NULL, "AMFI (SWEET stand-in)");
static int sweet_developer_mode = 1;
static int sweet_security_research;
SYSCTL_INT(_security_mac_amfi, OID_AUTO, developer_mode_status, CTLFLAG_RD | CTLFLAG_LOCKED,
    &sweet_developer_mode, 0, "Developer Mode status");
SYSCTL_INT(_security_mac_amfi, OID_AUTO, developer_mode_resolved, CTLFLAG_RD | CTLFLAG_LOCKED,
    &sweet_developer_mode, 0, "Developer Mode resolved");
SYSCTL_INT(_hw_features, OID_AUTO, allows_security_research, CTLFLAG_RD | CTLFLAG_LOCKED,
    &sweet_security_research, 0, "security research device");

/* Called from bsd_init once MAC policies can register, before any exec. */
void sweet_amfi_policy_register(void);

void
sweet_amfi_policy_register(void)
{
	int error = mac_policy_register(&sweet_amfi_mac_conf, &sweet_amfi_mac_handle, NULL);
	printf("SWEET AMFI: platform binary policy %s (%d)\n",
	    error ? "NOT registered" : "registered", error);
	error = mac_policy_register(&sweet_sandbox_mac_conf, &sweet_sandbox_mac_handle, NULL);
	printf("SWEET Sandbox: permissive policy %s (%d)\n",
	    error ? "NOT registered" : "registered", error);
	error = mac_policy_register(&sweet_quarantine_mac_conf, &sweet_quarantine_mac_handle, NULL);
	printf("SWEET Quarantine: permissive policy %s (%d)\n",
	    error ? "NOT registered" : "registered", error);
}
#endif /* ARM64_BOARD_CONFIG_SWEET || ARM64_BOARD_CONFIG_A5XELTE */
