/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup pvfs2linux
 *
 *  Linux VFS Access Control List callbacks.
 *  This owes quite a bit of code to the ext2 acl code
 *  with appropriate modifications necessary for PVFS2.
 *  Currently works only for 2.6 kernels. No reason why it should
 *  not work for 2.4 kernels, but I am way too lazy to add that right now.
 */

#include "pvfs2-kernel.h"
#include "pvfs2-bufmap.h"

#if !defined(PVFS2_LINUX_KERNEL_2_4) && defined(HAVE_GENERIC_GETXATTR) && defined(CONFIG_FS_POSIX_ACL)
#include "pvfs2-internal.h"

#ifdef HAVE_POSIX_ACL_H
#include <linux/posix_acl.h>
#endif
#ifdef HAVE_LINUX_POSIX_ACL_XATTR_H
#include <linux/posix_acl_xattr.h>
#endif
#include <linux/xattr.h>
#ifdef HAVE_LINUX_XATTR_ACL_H
#include <linux/xattr_acl.h>
#endif
#include "bmi-byteswap.h"
#include <linux/fs_struct.h>

/*
 * Encoding and Decoding the extended attributes so that we can
 * retrieve it properly on any architecture.
 * Should these go any faster by making them as macros?
 * Probably not in the fast-path though...
 */

/*
 * Routines that retrieve and/or set ACLs for PVFS2 files.
 */
struct posix_acl *pvfs2_get_acl(struct inode *inode, int type)
{
    struct posix_acl *acl;
    int ret;
    char *key = NULL, *value = NULL;
    char *s;

    /* Won't work if you don't mount with the right set of options */
    if (get_acl_flag(inode) == 0) 
    {
        gossip_debug(GOSSIP_ACL_DEBUG,
                     "pvfs2_get_acl: ACL options disabled on this FS!\n");
        return NULL;
    }
    switch (type)
    {
        case ACL_TYPE_ACCESS:
            key = PVFS2_XATTR_NAME_ACL_ACCESS;
            break;
        case ACL_TYPE_DEFAULT:
            key = PVFS2_XATTR_NAME_ACL_DEFAULT;
            break;
        default:
            gossip_err("pvfs2_get_acl: bogus value of type %d\n", type);
            return ERR_PTR(-EINVAL);
    }
    /*
     * Rather than incurring a network call just to determine
     * the exact length of
     * the attribute, I just allocate a max length to save on the network call
     * Conceivably, we could pass NULL to pvfs2_inode_getxattr()
     * to probe the length
     * of the value, but I don't do that for now.
     */
    value = (char *) kmalloc(PVFS_MAX_XATTR_VALUELEN, GFP_KERNEL);
    if (value == NULL)
    {
        gossip_err("pvfs2_get_acl: Could not allocate value ptr\n");
        return ERR_PTR(-ENOMEM);
    }
    s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
    gossip_debug(GOSSIP_ACL_DEBUG, "inode %s, key %s, type %d\n", 
                 k2s(get_khandle_from_ino(inode),s), key, type);
    kfree(s);
    ret = pvfs2_inode_getxattr(inode, "", key, value, PVFS_MAX_XATTR_VALUELEN);
    /* if the key exists, convert it to an in-memory rep */
    if (ret > 0)
    {
#ifdef HAVE_POSIX_ACL_USER_NAMESPACE
        acl = posix_acl_from_xattr(&init_user_ns, value, ret);
#else
        acl = posix_acl_from_xattr(value, ret);
#endif
    }
    else if (ret == -ENODATA || ret == -ENOSYS)
    {
        acl = NULL;
    }
    else
    {
        s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
        gossip_err("inode %s retrieving acl's failed with error %d\n",
                   k2s(get_khandle_from_ino(inode),s), ret);
        kfree(s);
        acl = ERR_PTR(ret);
    }
    if (value)
    {
        kfree(value);
    }
    return acl;
}

static int pvfs2_set_acl(struct inode *inode, int type, struct posix_acl *acl)
{
    int error = 0;
    void *value = NULL;
    size_t size = 0;
    const char *name = NULL;
    pvfs2_inode_t *pvfs2_inode = PVFS2_I(inode);
    char *s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);

    /* We dont't allow this on a symbolic link */
    if (S_ISLNK(inode->i_mode))
    {
        gossip_err("pvfs2_set_acl: disallow on symbolic links\n");
        return -EACCES;
    }
    /* if ACL option is not set, then we return early */
    if (get_acl_flag(inode) == 0)
    {
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_set_acl: ACL options disabled on"
                "this FS!\n");
        return 0;
    }
    switch (type)
    {
        case ACL_TYPE_ACCESS:
        {
            name = PVFS2_XATTR_NAME_ACL_ACCESS;
            if (acl) 
            {
#ifdef HAVE_POSIX_ACL_EQUIV_MODE_UMODE_T
                umode_t mode = inode->i_mode;
#else
                mode_t mode = inode->i_mode;
#endif /* HAVE_POSIX_ACL_EQUIV_MODE_UMODE_T */ 
                /* can we represent this with the UNIXy permission bits? */
                error = posix_acl_equiv_mode(acl, &mode);
                /* uh oh some error.. */
                if (error < 0) 
                {
                    gossip_err("pvfs2_set_acl: posix_acl_equiv_mode error %d\n", 
                            error);
                    return error;
                }
                else /* okay, go ahead and do just that */
                {
                    if (inode->i_mode != mode)
                    {
                        SetModeFlag(pvfs2_inode);
                    }
                    inode->i_mode = mode;
                    mark_inode_dirty_sync(inode);
                    if (error == 0) /* equivalent. so dont set acl! */
                    {
                        acl = NULL;
                    }
                }
            }
            break;
        }
        case ACL_TYPE_DEFAULT:
        {
            name = PVFS2_XATTR_NAME_ACL_DEFAULT;
            /* Default ACLs cannot be set/modified for non-directory objects! */
            if (!S_ISDIR(inode->i_mode))
            {
                gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_set_acl: setting default "
                             "ACLs on non-dir object? %s\n",
                             acl ? "disallowed" : "ok");
                return acl ? -EACCES : 0;
            }
            break;
        }
        default:
        {
            gossip_err("pvfs2_set_acl: invalid type %d!\n", type);
            return -EINVAL;
        }
    }
    gossip_debug(GOSSIP_ACL_DEBUG,
                 "pvfs2_set_acl: inode %s, key %s type %d\n",
                 k2s(get_khandle_from_ino(inode),s), name, type);
    kfree(s);
    /* If we do have an access control list, then we need to encode that! */
    if (acl) 
    {
        value = (char *) kmalloc(PVFS_MAX_XATTR_VALUELEN, GFP_KERNEL);
        if (IS_ERR(value)) 
        {
            return (int) PTR_ERR(value);
        }
#ifdef HAVE_POSIX_ACL_USER_NAMESPACE
        size = posix_acl_to_xattr(&init_user_ns, 
                                  acl, 
                                  value, 
                                  PVFS_MAX_XATTR_VALUELEN);
#else
        size = posix_acl_to_xattr(acl, value, PVFS_MAX_XATTR_VALUELEN);
#endif
        if (size < 0)
        {
            error = size;
            goto errorout;
        }
    }
    gossip_debug(GOSSIP_ACL_DEBUG,
                 "pvfs2_set_acl: name %s, value %p, size %zd, "
                 " acl %p\n", name, value, size, acl);
    /* Go ahead and set the extended attribute now 
     * NOTE: Suppose acl was NULL, then value will be NULL and 
     * size will be 0 and that will xlate to a removexattr.
     * However, we dont want removexattr complain if attributes
     * does not exist.
     */
    error = pvfs2_inode_setxattr(inode, "", name, value, size, 0);

errorout:
    if (value) 
    {
        kfree(value);
    }
    return error;
}

static int pvfs2_xattr_get_acl(struct inode *inode,
                               int type,
                               void *buffer,
                               size_t size)
{
    struct posix_acl *acl;
    int error;

    /* if we have not been mounted with acl option, ignore this */
    if (get_acl_flag(inode) == 0)
    {
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_xattr_get_acl: ACL options "
                     "disabled on this FS!\n");
        return -EOPNOTSUPP;
    }
    acl = pvfs2_get_acl(inode, type);
    if (IS_ERR(acl))
    {
        error = PTR_ERR(acl);
        gossip_err("pvfs2_get_acl failed with error %d\n", error);
        goto out;
    }
    if (acl == NULL)
    {
        error = -ENODATA;
        goto out;
    }
#ifdef HAVE_POSIX_ACL_USER_NAMESPACE
    error = posix_acl_to_xattr(&init_user_ns, acl, buffer, size);
#else
    error = posix_acl_to_xattr(acl, buffer, size);
#endif
    posix_acl_release(acl);
    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_xattr_get_acl: posix_acl_to_xattr "
                 "returned %d\n", error);
out:
    return error;
}

static int pvfs2_xattr_get_acl_access(
#ifdef HAVE_XATTR_HANDLER_GET_4_4
                                      const struct xattr_handler *handler,
                                      struct dentry *dentry,
                                      const char *name,
                                      void *buffer,
                                      size_t size)
#elif defined(HAVE_XATTR_HANDLER_GET_2_6_33)
                                      struct dentry *dentry,
                                      const char *name,
                                      void *buffer,
                                      size_t size,
                                      int handler_flag)

#else
                                      struct inode *inode,
                                      const char *name,
                                      void *buffer,
                                      size_t size)
#endif
{
    gossip_debug(GOSSIP_ACL_DEBUG, "%s: %s\n", __func__, name);

    if (strcmp(name, "") != 0)
    {
        gossip_err("%s invalid name %s\n", __func__, name);
        return -EINVAL;
    }

#if defined(HAVE_XATTR_HANDLER_GET_4_4) || \
    defined(HAVE_XATTR_HANDLER_GET_2_6_33)
    return pvfs2_xattr_get_acl(dentry->d_inode,
#else
    return pvfs2_xattr_get_acl(inode,
#endif
                               ACL_TYPE_ACCESS,
                               buffer,
                               size);
}

static int pvfs2_xattr_get_acl_default(
#ifdef HAVE_XATTR_HANDLER_GET_4_4
                                      const struct xattr_handler *handler,
                                      struct dentry *dentry,
                                      const char *name,
                                      void *buffer,
                                      size_t size)
#elif defined(HAVE_XATTR_HANDLER_GET_2_6_33)
                                      struct dentry *dentry,
                                      const char *name,
                                      void *buffer,
                                      size_t size,
                                      int handler_flag)

#else
                                      struct inode *inode,
                                      const char *name,
                                      void *buffer,
                                      size_t size)
#endif
{
    gossip_debug(GOSSIP_ACL_DEBUG, "%s: %s\n", __func__, name);

    if (strcmp(name, "") != 0)
    {
        gossip_err("%s: invalid name %s\n", __func__, name);
        return -EINVAL;
    }

#if defined(HAVE_XATTR_HANDLER_GET_4_4) || \
    defined(HAVE_XATTR_HANDLER_GET_2_6_33)
    return pvfs2_xattr_get_acl(dentry->d_inode,
#else
    return pvfs2_xattr_get_acl(inode,
#endif
                               ACL_TYPE_ACCESS,
                               buffer,
                               size);
}

static int pvfs2_xattr_set_acl(struct inode *inode,
                               int type,
                               const void *value,
                               size_t size)
{
    struct posix_acl *acl;
    int error;
#ifdef HAVE_FROM_KUID
    int fsuid = from_kuid(&init_user_ns, current_fsuid());
#elif defined(HAVE_CURRENT_FSUID)
    int fsuid = current_fsuid();
#else
    int fsuid = current->fsuid;
#endif

    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_xattr_set_acl called with size %ld\n",
            (long)size);
    /* if we have not been mounted with acl option, ignore this */
    if (get_acl_flag(inode) == 0)
    {
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_xattr_set_acl: ACL options "
                "disabled on this FS!\n");
        return -EOPNOTSUPP;
    }
    /* Are we capable of setting acls on a file for which we should not be? */
#ifdef HAVE_FROM_KUID
    if ((fsuid != from_kuid(&init_user_ns,inode->i_uid)) && 
        !capable(CAP_FOWNER))
#else
    if ((fsuid != inode->i_uid) && !capable(CAP_FOWNER))
#endif
    {
        gossip_err("pvfs2_xattr_set_acl: operation not permitted "
                "(current->fsuid %d), (inode->owner %d)\n", 
                fsuid,
#ifdef HAVE_FROM_KUID
                from_kuid(&init_user_ns,inode->i_uid));
#else
		inode->i_uid);
#endif
        return -EPERM;
    }
    if (value) 
    {
#ifdef HAVE_POSIX_ACL_USER_NAMESPACE
        acl = posix_acl_from_xattr(&init_user_ns, value, size);
#else
        acl = posix_acl_from_xattr(value, size);
#endif
        if (IS_ERR(acl))
        {
            error = PTR_ERR(acl);
            gossip_err("pvfs2_xattr_set_acl: posix_acl_from_xattr returned "
                    "error %d\n", error);
            goto err;
        }
        else if (acl) 
        {
#ifdef HAVE_POSIX_ACL_VALID_USER_NAMESPACE
            error = posix_acl_valid(&init_user_ns, acl);
#else
            error = posix_acl_valid(acl);
#endif            
            if (error)
            {
                gossip_err("pvfs2_xattr_set_acl: posix_acl_valid returned "
                        "error %d\n", error);
                goto out;
            }
        }
    }
    else
    {
        acl = NULL;
    }
    error = pvfs2_set_acl(inode, type, acl);
    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_set_acl returned error %d\n", error);
out:
    posix_acl_release(acl);
err:
    return error;
}


static int pvfs2_xattr_set_acl_access(
#ifdef HAVE_XATTR_HANDLER_SET_4_4
                                      const struct xattr_handler *handler,
                                      struct dentry *dentry,
                                      const char *name, 
                                      const void *buffer, 
                                      size_t size, 
                                      int flags)
#elif defined (HAVE_XATTR_HANDLER_SET_2_6_33)
                                      struct dentry *dentry,
                                      const char *name, 
                                      const void *buffer, 
                                      size_t size, 
                                      int flags,
                                      int handler_flags)
#else /* pre 2.6.33 */
                                      struct inode *inode, 
                                      const char *name,
                                      const void *buffer,
                                      size_t size,
                                      int flags)
#endif
{
    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_xattr_set_acl_access: %s\n", name);
    if (strcmp(name, "") != 0)
    {
        gossip_err("set_acl_access invalid name %s\n", name);
        return -EINVAL;
    }
#if defined(HAVE_XATTR_HANDLER_SET_4_4) || \
    defined(HAVE_XATTR_HANDLER_SET_2_6_33)
    return pvfs2_xattr_set_acl(dentry->d_inode, ACL_TYPE_ACCESS, buffer, size);
#else /* pre 2.6.33 */
    return pvfs2_xattr_set_acl(inode, ACL_TYPE_ACCESS, buffer, size);
#endif
}

static int pvfs2_xattr_set_acl_default(
#ifdef HAVE_XATTR_HANDLER_SET_4_4
                                       const struct xattr_handler *handler,
                                       struct dentry *dentry,
                                       const char *name, 
                                       const void *buffer, 
                                       size_t size, 
                                       int flags)
#elif defined (HAVE_XATTR_HANDLER_SET_2_6_33)
                                       struct dentry *dentry,
                                       const char *name,
                                       const void *buffer,
                                       size_t size,
                                       int flags,
                                       int handler_flags)
#else /* pre 2.6.33 */
                                       struct inode *inode,
                                       const char *name,
                                       const void *buffer,
                                       size_t size,
                                       int flags)
#endif
{
    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_xattr_set_acl_default: %s\n", name);
    if (strcmp(name, "") != 0)
    {
        gossip_err("set_acl_default invalid name %s\n", name);
        return -EINVAL;
    }
#if defined(HAVE_XATTR_HANDLER_SET_4_4) || \
    defined(HAVE_XATTR_HANDLER_SET_2_6_33)
    return pvfs2_xattr_set_acl(dentry->d_inode, ACL_TYPE_DEFAULT, buffer, size);
#else /* pre 2.6.33 */
    return pvfs2_xattr_set_acl(inode, ACL_TYPE_DEFAULT, buffer, size);
#endif
}

struct xattr_handler pvfs2_xattr_acl_access_handler = {
    .prefix = PVFS2_XATTR_NAME_ACL_ACCESS,
    .get    = pvfs2_xattr_get_acl_access,
    .set    = pvfs2_xattr_set_acl_access,
};

struct xattr_handler pvfs2_xattr_acl_default_handler = {
    .prefix = PVFS2_XATTR_NAME_ACL_DEFAULT,
    .get    = pvfs2_xattr_get_acl_default,
    .set    = pvfs2_xattr_set_acl_default,
};

/*
 * initialize the ACLs of a new inode.
 * This needs to be called from pvfs2_get_custom_inode.
 * Note that for the root of the PVFS2 file system,
 * dir will be NULL! For all others dir will be non-NULL
 * However, inode cannot be NULL!
 * Returns 0 on success and -ve number on failure.
 */
int pvfs2_init_acl(struct inode *inode, struct inode *dir)
{
    struct posix_acl *acl = NULL;
    int error = 0;
    pvfs2_inode_t *pvfs2_inode = PVFS2_I(inode);
#if defined(HAVE_POSIX_ACL_CREATE_3) 
        umode_t mode;
#elif defined(HAVE_POSIX_ACL_CREATE_4)
        umode_t mode = inode->i_mode;
        struct posix_acl *default_acl;
#elif defined(HAVE_POSIX_ACL_CLONE)
        struct posix_acl *clone = NULL;
        mode_t mode;
#else
	#error No posix_acl_create or posix_acl_clone defined
#endif /* HAVE_POSIX_ACL_CREATE_3 */ 

    if (dir == NULL)
        dir = inode;
    ClearModeFlag(pvfs2_inode);
    if (!S_ISLNK(inode->i_mode))
    {
        if (get_acl_flag(inode) == 1)
        {
            acl = pvfs2_get_acl(dir, ACL_TYPE_DEFAULT);
            if (IS_ERR(acl)) {
                error = PTR_ERR(acl);
                gossip_err("pvfs2_get_acl (default) failed with error %d\n", error);
                return error;
            }
        }
        if (!acl && dir != inode)
        {
            int old_mode = inode->i_mode;
            inode->i_mode &= ~current->fs->umask;
            gossip_debug(GOSSIP_ACL_DEBUG, "inode->i_mode before %o and "
                    "after %o\n", old_mode, inode->i_mode);
            if (old_mode != inode->i_mode)
                SetModeFlag(pvfs2_inode);
        }
    }
    if (get_acl_flag(inode) == 1 && acl)
    {
        if (S_ISDIR(inode->i_mode)) 
        {
            error = pvfs2_set_acl(inode, ACL_TYPE_DEFAULT, acl);
            if (error) {
                gossip_err("pvfs2_set_acl (default) directory failed with "
                        "error %d\n", error);
                ClearModeFlag(pvfs2_inode);
                goto cleanup;
            }
        }
#ifdef HAVE_POSIX_ACL_CREATE_3
        error = posix_acl_create(&acl, GFP_KERNEL, &mode);
#elif defined(HAVE_POSIX_ACL_CREATE_4)
        error = posix_acl_create(dir, &mode, &default_acl, &acl);
#elif defined(HAVE_POSIX_ACL_CLONE)
        clone = posix_acl_clone(acl, GFP_KERNEL);
        error = -ENOMEM;
        if (!clone) {
            gossip_err("posix_acl_clone failed with ENOMEM\n");
            ClearModeFlag(pvfs2_inode);
            goto cleanup;
        }
        mode = inode->i_mode;
        error = posix_acl_create_masq(clone, &mode);
#else
	#error No posix_acl_create or posix_acl_clone defined
#endif /* HAVE_POSIX_ACL_CREATE_3 */
        if (error >= 0)
        {
#ifdef HAVE_POSIX_ACL_CREATE_3
            gossip_debug(GOSSIP_ACL_DEBUG, "posix_acl_create changed mode "
                    "from %o to %o\n", inode->i_mode, mode);
#elif defined(HAVE_POSIX_ACL_CREATE_4)
            if (mode != inode->i_mode) {
               gossip_debug(GOSSIP_ACL_DEBUG,
                 "posix_acl_create changed mode from %o to %o\n",
                 inode->i_mode,
                 mode);
            }
#else
            gossip_debug(GOSSIP_ACL_DEBUG, "posix_acl_create_masq changed mode "
                    "from %o to %o\n", inode->i_mode, mode);
#endif /* HAVE_POSIX_ACL_CREATE_3 */
            /*
             * Dont do a needless ->setattr() if mode has not changed 
             */
            if (inode->i_mode != mode)
                SetModeFlag(pvfs2_inode);
            inode->i_mode = mode;
            /* 
             * if this is an ACL that cannot be captured by
             * the mode bits, go for the server! 
             */
            if (error > 0)
            {
#ifdef HAVE_POSIX_ACL_CREATE_3
                error = pvfs2_set_acl(inode, ACL_TYPE_ACCESS, acl);
#elif defined(HAVE_POSIX_ACL_CREATE_4)
                if (default_acl) {
                  error = pvfs2_set_acl(inode, ACL_TYPE_DEFAULT, default_acl);
                  posix_acl_release(default_acl);
                }

                if (acl) {
                  if (!error)
                     error = pvfs2_set_acl(inode, ACL_TYPE_ACCESS, acl);
                posix_acl_release(acl);
                }
#elif defined(HAVE_POSIX_ACL_CLONE)
                error = pvfs2_set_acl(inode, ACL_TYPE_ACCESS, clone);
#else 
	#error No posix_acl_create or posix_acl_clone defined
#endif /* HAVE_POSIX_ACL_CREATE_3 */
                gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_set_acl (access) returned %d\n", error);
            }
        }
#ifdef HAVE_POSIX_ACL_CLONE
        posix_acl_release(clone);
#endif /* HAVE_POSIX_ACL_CREATE_3 */
    }
    /* If mode of the inode was changed, then do a forcible ->setattr */
#ifdef HAVE_POSIX_ACL_CREATE_4
    if (mode != inode->i_mode) {
       SetModeFlag(pvfs2_inode);
       inode->i_mode = mode;
       pvfs2_flush_inode(inode);
    }
#else
    if (ModeFlag(pvfs2_inode))
       pvfs2_flush_inode(inode);
#endif
cleanup:
#ifndef HAVE_POSIX_ACL_CREATE_4
    posix_acl_release(acl);
#endif
    return error;
}

/*
 * Handles the case when a chmod is done for an inode that may have an
 * access control list.
 * The inode->i_mode field is updated to the desired value by the caller
 * before calling this function which returns 0 on success and a -ve
 * number on failure.
 */
int pvfs2_acl_chmod(struct inode *inode)
{
    struct posix_acl *acl = NULL;
#ifdef HAVE_POSIX_ACL_CLONE
    struct posix_acl *clone = NULL;
#endif /* HAVE_POSIX_ACL_CLONE */
    int error;

    if (get_acl_flag(inode) == 0)
    {
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_acl_chmod: ACL options "
                "disabled on this FS!\n");
        return 0;
    }
    if (S_ISLNK(inode->i_mode))
    {
        gossip_err("pvfs2_acl_chmod: operation not permitted on symlink!\n");
        error = -EACCES;
        goto out;
    }
    acl = pvfs2_get_acl(inode, ACL_TYPE_ACCESS);
    if (IS_ERR(acl))
    {
        error = PTR_ERR(acl);
        gossip_err("pvfs2_acl_chmod: get acl (access) failed with %d\n", error);
        goto out;
    }
    if(!acl)
    {
        error = 0;
        goto out;
    }
#ifdef HAVE_POSIX_ACL_CHMOD_3
    error = posix_acl_chmod(&acl, GFP_KERNEL, inode->i_mode);
#elif defined(HAVE_POSIX_ACL_CHMOD_2)
    error = posix_acl_chmod(inode, inode->i_mode);
#else
    error = posix_acl_chmod_masq(acl, inode->i_mode);
#endif
    if (!error)
    {
        error = pvfs2_set_acl(inode, ACL_TYPE_ACCESS, acl);
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_acl_chmod: pvfs2 set acl "
                "(access) returned %d\n", error);
    }
#ifdef HAVE_POSIX_CLONE
    clone = posix_acl_clone(acl, GFP_KERNEL);
    if (!clone)
    {
        gossip_err("pvfs2_acl_chmod failed with ENOMEM\n");
        error = -ENOMEM;
        goto out;
    }
#endif /* HAVE_POSIX_CLONE */

#ifdef HAVE_POSIX_ACL_CHMOD_3
    error = posix_acl_chmod(&acl, GFP_KERNEL, inode->i_mode);
    if (!error)
    {
        error = pvfs2_set_acl(inode, ACL_TYPE_ACCESS, acl);
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_acl_chmod: pvfs2 set acl "
                      "(access) returned %d\n", error);
    }
#elif defined(HAVE_POSIX_ACL_CHMOD_2)
    error = posix_acl_chmod(inode, inode->i_mode);
    if (!error)
    {
    }
#elif defined(HAVE_POSIX_ACL_CLONE)
    error = posix_acl_chmod_masq(clone, inode->i_mode);
    if (!error)
    {
        error = pvfs2_set_acl(inode, ACL_TYPE_ACCESS, clone);
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_acl_chmod: pvfs2 set acl "
                     "(access) returned %d\n", error);
    }
    posix_acl_release(clone);
#else
	#error No posix_acl_chmod or posix_acl_clone defined
#endif

out:
    posix_acl_release(acl);
    return error;
}

#if defined(HAVE_THREE_PARAM_GENERIC_PERMISSION) || \
	defined(HAVE_FOUR_PARAM_GENERIC_PERMISSION)
static int pvfs2_check_acl(struct inode *inode, int mask
# ifdef HAVE_THREE_PARAM_ACL_CHECK
                           , unsigned int flags
# endif /* HAVE_THREE_PARAM_ACL_CHECK */
                           )
{
    struct posix_acl *acl = NULL;
    char *s;

    s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
    gossip_debug(GOSSIP_ACL_DEBUG,
                 "pvfs2_check_acl: called on inode %s\n",
                 k2s(get_khandle_from_ino(inode),s));
    kfree(s);

    acl = pvfs2_get_acl(inode, ACL_TYPE_ACCESS);

    if (IS_ERR(acl))
    {
        int error = PTR_ERR(acl);
        gossip_debug(GOSSIP_ACL_DEBUG,
                     "pvfs2_check_acl: pvfs2_get_acl returned error %d\n",
                     error);
        return error;
    }
    if (acl) 
    {
        int error = posix_acl_permission(inode, acl, mask);
        posix_acl_release(acl);
        s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
        gossip_debug(GOSSIP_ACL_DEBUG,
                     "pvfs2_check_acl: posix_acl_permission "
                     "(inode %s, acl %p, mask %x) returned %d\n",
                     k2s(get_khandle_from_ino(inode),s),
                     acl,
                     mask,
                     error);
        kfree(s);
        return error;
    }
    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_check_acl returning EAGAIN\n");
    return -EAGAIN;
}
#endif


#ifdef HAVE_TWO_PARAM_PERMISSION
int pvfs2_permission(struct inode *inode, int mask)
#else
int pvfs2_permission(struct inode *inode,
                     int mask, 
# ifdef HAVE_THREE_PARAM_PERMISSION_WITH_FLAG
                     unsigned int flags)
# else
                     struct nameidata *nd)
# endif /* HAVE_THREE_PARAM_PERMISSION_WITH_FLAG */
#endif /* HAVE_TWO_PARAM_PERMISSION */
{
    char *s;
#ifdef HAVE_FROM_KUID
    int fsuid = from_kuid(&init_user_ns, current_fsuid());
#elif defined(HAVE_CURRENT_FSUID)
    int fsuid = current_fsuid();
#else
    int fsuid = current->fsuid;
#endif

#ifdef HAVE_GENERIC_PERMISSION
    int ret;

# if defined(HAVE_TWO_PARAM_GENERIC_PERMISSION)
    ret = generic_permission(inode, mask); 
# elif defined(HAVE_THREE_PARAM_GENERIC_PERMISSION)
    ret = generic_permission(inode, mask, pvfs2_check_acl); 
# elif defined(HAVE_FOUR_PARAM_GENERIC_PERMISSION)
    ret = generic_permission(inode, mask, 0, pvfs2_check_acl); 
# else
    #error generic_permission has an unknown number of parameters
# endif
    if (ret != 0)
    {
        s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
        gossip_debug(GOSSIP_ACL_DEBUG,
                     "pvfs2_permission failed: inode: %s mask = %o"
                     "mode = %o current->fsuid = %d "
                     "inode->i_uid = %d, inode->i_gid = %d "
                     "in_group_p = %d "
                     "(ret = %d)\n",
                     k2s(get_khandle_from_ino(inode),s),
                     mask,
                     inode->i_mode,
                     fsuid,
#ifdef HAVE_FROM_KUID
                     from_kuid(&init_user_ns,inode->i_uid),
                     from_kgid(&init_user_ns,inode->i_gid),
#else
                     inode->i_uid,
                     inode->i_gid, 
#endif
                     in_group_p(inode->i_gid),
                     ret);
        kfree(s);
        gossip_debug(GOSSIP_ACL_DEBUG,
                     "pvfs2_permission: mode [%o] & mask [%o] "
                     " & S_IRWXO [%o] = %o == mask [%o]?\n", 
                     inode->i_mode, mask, S_IRWXO, 
                     (inode->i_mode & mask & S_IRWXO), mask);
        gossip_debug(GOSSIP_ACL_DEBUG,
                     "pvfs2_permission: did we check ACL's? "
                     "(mode & S_IRWXG = %d)\n",
                     inode->i_mode & S_IRWXG);
    }
    else
    {
        s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
        gossip_debug(GOSSIP_ACL_DEBUG,
                     "pvfs2_permission succeeded on inode %s\n",
                     k2s(get_khandle_from_ino(inode),s));
        kfree(s);
    }
    return ret;
#else 
    /* We sort of duplicate the code below from generic_permission. */
    int mode = inode->i_mode;
    int error;

    s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_permission: inode: %s mask = %o"
                 "mode = %o current->fsuid = %d "
                 "inode->i_uid = %d, inode->i_gid = %d"
                 "in_group_p = %d\n", 
                 k2s(get_handle_from_ino(inode),s),
                 mask,
                 mode,
                 fsuid,
                 inode->i_uid,
                 inode->i_gid,
                 in_group_p(inode->i_gid));
    kfree(s);

    /* No write access on a rdonly FS */
    if ((mask & MAY_WRITE) && IS_RDONLY(inode) &&
            (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))
    {
        gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_permission: cannot write to a "
                     "read-only-file-system!\n");
        return -EROFS;
    }
    /* No write access to any immutable files */
    if ((mask & MAY_WRITE) && IS_IMMUTABLE(inode)) 
    {
        gossip_err("pvfs2_permission: cannot write to an immutable file!\n");
        return -EACCES;
    }
    if (fsuid == inode->i_uid) 
    {
        mode >>= 6;
    }
    else 
    {
        if (get_acl_flag(inode) == 1) 
        {
            /*
             * Access ACL won't work if we don't have group permission bits
             * set on the file!
             */
            if (!(mode & S_IRWXG))
            {
                goto check_groups;
            }
            error = pvfs2_check_acl(inode, mask);
            /* ACL disallows access */
            if (error == -EACCES) 
            {
                gossip_debug(GOSSIP_ACL_DEBUG,
                             "pvfs2_permission: acl disallowing "
                             "access to file\n");
                goto check_capabilities;
            }
            /* No ACLs present? */
            else if (error == -EAGAIN) 
            {
                goto check_groups;
            }
            gossip_debug(GOSSIP_ACL_DEBUG,
                         "pvfs2_permission: returning %d\n",
                         error);
            /* Any other error */
            return error;
        }
check_groups:
        if (in_group_p(inode->i_gid))
        {
            mode >>= 3;
        }
    }
    if ((mode & mask & S_IRWXO) == mask)
    {
        return 0;
    }
    gossip_debug(GOSSIP_ACL_DEBUG,
                 "pvfs2_permission: mode (%o) & mask (%o) "
                 "& S_IRWXO (%o) = %o == mask (%o)?\n",
                 mode, mask, S_IRWXO, mode & mask & S_IRWXO, mask);
check_capabilities:
    /* Are we allowed to override DAC */
    if (!(mask & MAY_EXEC) || (inode->i_mode & S_IXUGO) ||
         S_ISDIR(inode->i_mode))
    {
        if(capable(CAP_DAC_OVERRIDE))
        {
            return 0;
        }
    }

    gossip_debug(GOSSIP_ACL_DEBUG, "pvfs2_permission: disallowing access\n");
    return -EACCES;
#endif /* HAVE_GENERIC_PERMISSION */
}

#endif
/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
