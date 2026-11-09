/*
 * The file systems of this port in bsd/vfs/vfs_conf.c's tables: RAMFS (a writable RAM disk), HFS (the data volume, bsd/hfs) and
 * APFS (the root, read only, bsd/miscfs/apfs). vfs_conf.c includes this four times, each with HAKIOS_VFS_PART set to what it
 * wants here: 1 the declarations, 2 the entries of vfstbllist[] (before mockfs, which has to stay the last), 3 the declarations
 * of the vnode operations, 4 their entries in vfs_opv_descs[].
 */
#ifndef HAKIOS_VFS_PART
#error "HAKIOS_VFS_PART is not set"
#endif

#if HAKIOS_VFS_PART == 1
#if RAMFS
extern  struct vfsops ramfs_vfsops;
#endif /* RAMFS */

#if HFS
extern  struct vfsops hfs_vfsops;
#endif /* HFS */

#if APFS
extern  struct vfsops apfs_vfsops;
extern  int apfs_mountroot(mount_t, vnode_t, vfs_context_t);
#endif /* APFS */

enum hakios_fs_type_num {
	FT_RAMFS   = 0x72616D66,
	FT_HFS     = 0x11,
	FT_APFS    = 0x61706673
};

#elif HAKIOS_VFS_PART == 2
#if RAMFS
	/* Read-write filesystem in memory (no device, no mount arguments) */
	{
		.vfc_vfsops = &ramfs_vfsops,
		.vfc_name = "ramfs",
		.vfc_typenum = FT_RAMFS,
		.vfc_refcount = 0,
		.vfc_flags = MNT_LOCAL,
		.vfc_mountroot = NULL,
		.vfc_next = NULL,
		.vfc_reserved1 = 0,
		.vfc_reserved2 = 0,
		.vfc_vfsflags = VFC_VFSGENERICARGS | VFC_VFS64BITREADY | VFC_VFSNATIVEXATTR,
		.vfc_descptr = NULL,
		.vfc_descsize = 0,
		.vfc_sysctl = NULL
	},
#endif /* RAMFS */

#if HFS
	/*
	 * HFS Plus, read-write, built in rather than a kext (bsd/hfs, Apple's hfs-627.100.6): the data volume on the
	 * phone's flash, /private/var, where ramfs keeps it in memory.  Its own arguments (struct hfs_mount_args) and
	 * local ones, as the kext registered itself (no VFS_TBLGENERICMNTARGS): the kernel looks up the device named
	 * by their first field and hands its vnode to hfs_mount().  Mounted by init, from user space.
	 */
	{
		.vfc_vfsops = &hfs_vfsops,
		.vfc_name = "hfs",
		.vfc_typenum = FT_HFS,
		.vfc_refcount = 0,
		.vfc_flags = MNT_LOCAL | MNT_DOVOLFS,
		.vfc_mountroot = NULL,
		.vfc_next = NULL,
		.vfc_reserved1 = 0,
		.vfc_reserved2 = 0,
		.vfc_vfsflags = VFC_VFSLOCALARGS | VFC_VFSREADDIR_EXTENDED | VFC_VFS64BITREADY | VFC_VFSVNOP_PAGEOUTV2 |
		    VFC_VFSVNOP_PAGEINV2 | VFC_VFSNATIVEXATTR,
		.vfc_descptr = NULL,
		.vfc_descsize = 0,
		.vfc_sysctl = NULL
	},
#endif /* HFS */

#if APFS
	/*
	 * Apple File System, read only: the root and every image of this port's (the RAM
	 * disks are APFS containers).  Before mockfs, which accepts any root device: a device
	 * that is not APFS comes back EINVAL and vfs_mountroot() goes on to the next.
	 */
	{
		.vfc_vfsops = &apfs_vfsops,
		.vfc_name = "apfs",
		.vfc_typenum = FT_APFS,
		.vfc_refcount = 0,
		.vfc_flags = MNT_LOCAL | MNT_RDONLY,
		.vfc_mountroot = apfs_mountroot,
		.vfc_next = NULL,
		.vfc_reserved1 = 0,
		.vfc_reserved2 = 0,
		/*
		 * Generic args, not local: a kernel mount of a filesystem marked
		 * VFC_VFSLOCALARGS is refused outright (mount_common), and this one has to be
		 * mountable from the kernel.  So apfs_mount() takes the device's path as its
		 * mount data and opens it itself.
		 */
		.vfc_vfsflags = VFC_VFSGENERICARGS | VFC_VFS64BITREADY | VFC_VFSNATIVEXATTR,
		.vfc_descptr = NULL,
		.vfc_descsize = 0,
		.vfc_sysctl = NULL
	},
#endif /* APFS */

#elif HAKIOS_VFS_PART == 3
#if RAMFS
extern const struct vnodeopv_desc ramfs_vnodeop_opv_desc;
#endif /* RAMFS */

#if HFS
extern const struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern const struct vnodeopv_desc hfs_specop_opv_desc;
extern const struct vnodeopv_desc hfs_fifoop_opv_desc;
#endif /* HFS */

#if APFS
extern const struct vnodeopv_desc apfs_vnodeop_opv_desc;
#endif /* APFS */

#elif HAKIOS_VFS_PART == 4
#if RAMFS
	&ramfs_vnodeop_opv_desc,
#endif /* RAMFS */
#if HFS
	&hfs_vnodeop_opv_desc,
	&hfs_specop_opv_desc,
	&hfs_fifoop_opv_desc,
#endif /* HFS */
#if APFS
	&apfs_vnodeop_opv_desc,
#endif /* APFS */

#endif /* HAKIOS_VFS_PART */
#undef HAKIOS_VFS_PART
