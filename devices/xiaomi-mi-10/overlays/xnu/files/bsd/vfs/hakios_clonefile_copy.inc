/*
 * clonefile(2) on a file system that cannot clone: copy instead (included by vfs_syscalls.c,
 * used by clonefile_internal()).
 *
 * The data volume here (/private/var) is HFS Plus, not APFS, and HFS has no VNOP_CLONEFILE: the call
 * ends in ENOTSUP. iOS takes cloning for granted -- MobileInstallation stages every file of an
 * application it installs with clonefile() and gives up on the first ENOTSUP ("Failed to clonefile
 * .../PiliPlus.app/Icon.png ...: Operation not supported", MIInstallerErrorDomain 107) -- so for a
 * regular file or a symbolic link the kernel makes the same result the slow way: a new file, created
 * with the attributes clonefile_internal() prepared for the clone (owner, mode, ACL and the same
 * exclusive-create semantics: the name was looked up as not existing), then the data, the extended
 * attributes (HFS lists neither com.apple.system.* nor the compression ones, as for a clone), then
 * the times and the flags of the source. What a clone shares with its source is the only
 * difference, and nothing can tell. clonefile_internal() then finishes as after a clone (attribute
 * fallback, MAC label, name, fsevents). A directory still gets ENOTSUP: APFS clones a whole tree,
 * and the callers that clone directories (copyfile's COPYFILE_CLONE, MobileInstallation's
 * _copyItemAtURL) copy by themselves when the clone fails. On any error the partial copy is removed.
 */

#define CLONE_COPY_CHUNK        (128 * 1024)

static int
clone_copy_data(vnode_t fvp, vnode_t tvp, off_t size, vfs_context_t ctx)
{
	kauth_cred_t cred = vfs_context_ucred(ctx);
	proc_t p = vfs_context_proc(ctx);
	char *buf = kalloc_data(CLONE_COPY_CHUNK, Z_WAITOK);
	off_t off = 0;
	int error = 0;

	if (buf == NULL) {
		return ENOMEM;
	}
	while (off < size) {
		int len = (int)MIN((off_t)CLONE_COPY_CHUNK, size - off);
		int resid;

		error = vn_rdwr(UIO_READ, fvp, buf, len, off, UIO_SYSSPACE, IO_NODELOCKED, cred, &resid, p);
		if (error || resid == len) {
			break;          /* an error, or the source got shorter: copy what there was */
		}
		len -= resid;
		error = vn_rdwr(UIO_WRITE, tvp, buf, len, off, UIO_SYSSPACE, IO_NODELOCKED | IO_UNIT, cred, &resid, p);
		if (!error && resid != 0) {
			error = EIO;
		}
		if (error) {
			break;
		}
		off += len;
	}
	kfree_data(buf, CLONE_COPY_CHUNK);
	return error;
}

static int
clone_copy_xattrs(vnode_t fvp, vnode_t tvp, vfs_context_t ctx)
{
	uio_t uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
	size_t names_size = 0, size = 0;
	char *names = NULL, *name, *value;
	int error;

	error = vn_listxattr(fvp, NULL, &names_size, XATTR_NOSECURITY, ctx);
	if (error || names_size == 0) {
		goto out;
	}
	names = kalloc_data(names_size, Z_WAITOK);
	if (names == NULL) {
		error = ENOMEM;
		goto out;
	}
	uio_reset(uio, 0, UIO_SYSSPACE, UIO_READ);
	uio_addiov(uio, CAST_USER_ADDR_T(names), names_size);
	error = vn_listxattr(fvp, uio, &size, XATTR_NOSECURITY, ctx);
	if (error) {
		goto out;
	}
	size = names_size - (size_t)uio_resid(uio);
	for (name = names; name < names + size; name += strlen(name) + 1) {
		size_t value_size = 0, allocated;

		error = vn_getxattr(fvp, name, NULL, &value_size, XATTR_NOSECURITY, ctx);
		if (error) {
			break;
		}
		allocated = MAX(value_size, 1);
		value = kalloc_data(allocated, Z_WAITOK);
		if (value == NULL) {
			error = ENOMEM;
			break;
		}
		uio_reset(uio, 0, UIO_SYSSPACE, UIO_READ);
		uio_addiov(uio, CAST_USER_ADDR_T(value), value_size);
		error = vn_getxattr(fvp, name, uio, &value_size, XATTR_NOSECURITY, ctx);
		if (!error) {
			value_size -= (size_t)uio_resid(uio);
			uio_reset(uio, 0, UIO_SYSSPACE, UIO_WRITE);
			uio_addiov(uio, CAST_USER_ADDR_T(value), value_size);
			error = vn_setxattr(tvp, name, uio, XATTR_NOSECURITY, ctx);
		}
		kfree_data(value, allocated);
		if (error) {
			break;
		}
	}
out:
	if (names != NULL) {
		kfree_data(names, names_size);
	}
	uio_free(uio);
	return error;
}

static int
clonefile_by_copy(vnode_t fvp, vnode_t tdvp, vnode_t *tvpp, struct componentname *cnp,
    struct vnode_attr *nva, vfs_context_t ctx)
{
	static pid_t said;
	proc_t p = vfs_context_proc(ctx);
	struct vnode_attr va, tva;
	vnode_t tvp = NULLVP;
	char *target = NULL;
	int error;

	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_access_time);
	VATTR_WANTED(&va, va_modify_time);
	VATTR_WANTED(&va, va_create_time);
	if ((error = vnode_getattr(fvp, &va, ctx)) != 0) {
		return error;
	}

	/* the flags last: an immutable or append-only source would stop the copy into its own copy */
	VATTR_INIT(&tva);
	if (VATTR_IS_ACTIVE(nva, va_flags)) {
		/* the data is copied out decompressed, so the copy is not a compressed file */
		VATTR_SET(&tva, va_flags, nva->va_flags & ~UF_COMPRESSED);
		VATTR_CLEAR_ACTIVE(nva, va_flags);
	}

	if (vnode_vtype(fvp) == VLNK) {
		uio_t uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);

		target = zalloc(ZV_NAMEI);
		uio_addiov(uio, CAST_USER_ADDR_T(target), MAXPATHLEN - 1);
		error = VNOP_READLINK(fvp, uio, ctx);
		target[MAXPATHLEN - 1 - uio_resid(uio)] = 0;
		uio_free(uio);
		if (!error) {
			error = VNOP_SYMLINK(tdvp, &tvp, cnp, nva, target, ctx);
		}
		zfree(ZV_NAMEI, target);
	} else {
		error = VNOP_CREATE(tdvp, &tvp, cnp, nva, ctx);
		if (!error) {
			error = clone_copy_data(fvp, tvp, VATTR_IS_SUPPORTED(&va, va_data_size) ? (off_t)va.va_data_size : 0, ctx);
		}
	}
	if (error) {
		goto out;
	}

	error = clone_copy_xattrs(fvp, tvp, ctx);
	if (!error) {
		if (VATTR_IS_SUPPORTED(&va, va_access_time)) {
			VATTR_SET(&tva, va_access_time, va.va_access_time);
		}
		if (VATTR_IS_SUPPORTED(&va, va_modify_time)) {
			VATTR_SET(&tva, va_modify_time, va.va_modify_time);
		}
		if (VATTR_IS_SUPPORTED(&va, va_create_time)) {
			VATTR_SET(&tva, va_create_time, va.va_create_time);
		}
		error = vnode_setattr(tvp, &tva, ctx);
	}
out:
	if (error && tvp != NULLVP) {
		(void)VNOP_REMOVE(tdvp, tvp, cnp, 0, ctx);
		vnode_put(tvp);
		tvp = NULLVP;
	}
	if (error) {
		printf("clonefile: %s[%d] %s: copy on %s failed: %d\n", proc_best_name(p), proc_pid(p),
		    cnp->cn_nameptr, vnode_mount(tdvp)->mnt_vfsstat.f_fstypename, error);
	} else if (said != proc_pid(p)) {
		said = proc_pid(p);     /* once for each process in a row, not for each of its files */
		printf("clonefile: %s[%d] %s: copied on %s (no clones there)\n", proc_best_name(p), proc_pid(p),
		    cnp->cn_nameptr, vnode_mount(tdvp)->mnt_vfsstat.f_fstypename);
	}
	*tvpp = tvp;
	return error;
}
