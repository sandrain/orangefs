/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup pvfs2linux
 *
 *  Linux VFS directory operations.
 */

#include "pvfs2-kernel.h"
#include "pvfs2-bufmap.h"
#include "pvfs2-sysint.h"
#include "pvfs2-internal.h"

#define PVFS_ITERATE_NEXT     (INT32_MAX - 3)

typedef struct 
{
    int buffer_index;
    pvfs2_readdir_response_t readdir_response;
    void *dents_buf;
} readdir_handle_t;

/* decode routine needed by kmod to make sense of the shared page for readdirs */
static long decode_dirents(char *ptr, pvfs2_readdir_response_t *readdir) 
{
    int i;
    pvfs2_readdir_response_t *rd = (pvfs2_readdir_response_t *) ptr;
    char *buf = ptr;
    char **pptr = &buf;

    readdir->token = rd->token;
    readdir->pvfs_dirent_outcount = rd->pvfs_dirent_outcount;
    readdir->dirent_array =
      kmalloc(readdir->pvfs_dirent_outcount * sizeof(*readdir->dirent_array),
              GFP_KERNEL);
    if (readdir->dirent_array == NULL)
        return -ENOMEM;

    *pptr += offsetof(pvfs2_readdir_response_t, dirent_array);
    for (i = 0; i < readdir->pvfs_dirent_outcount; i++)
    {
        dec_string(pptr,
                   &readdir->dirent_array[i].d_name,
                   &readdir->dirent_array[i].d_length);
        readdir->dirent_array[i].khandle = *(PVFS_khandle *) *pptr;
        *pptr += 16;
    }
    return ((unsigned long) *pptr - (unsigned long) ptr);
}

static long readdir_handle_ctor(readdir_handle_t *rhandle, void *buf, int buffer_index)
{
    long ret;

    if (buf == NULL)
    {
        gossip_err("Invalid NULL buffer specified in readdir_handle_ctor\n");
        return -ENOMEM;
    }
    if (buffer_index < 0)
    {
        gossip_err("Invalid buffer index specified in readdir_handle_ctor\n");
        return -EINVAL;
    }
    rhandle->buffer_index = buffer_index;
    rhandle->dents_buf = buf;
    if ((ret = decode_dirents(buf, &rhandle->readdir_response)) < 0)
    {
        gossip_err("Could not decode readdir from buffer %ld\n", ret);
        readdir_index_put(rhandle->buffer_index);
        rhandle->buffer_index = -1;
        gossip_debug(GOSSIP_DIR_DEBUG, "vfree %p\n", buf);
        vfree(buf);
        rhandle->dents_buf = NULL;
    }
    return ret;
}

static void readdir_handle_dtor(readdir_handle_t *rhandle)
{
    if (rhandle == NULL)
    {
        return;
    }
    if (rhandle->readdir_response.dirent_array)
    {
        kfree(rhandle->readdir_response.dirent_array);
        rhandle->readdir_response.dirent_array = NULL;
    }
    if (rhandle->buffer_index >= 0)
    {
        gossip_debug(GOSSIP_BUFMAP_DEBUG, "%s: put index:%d:\n",
               __func__,
               rhandle->buffer_index);
        readdir_index_put(rhandle->buffer_index);
        rhandle->buffer_index = -1;
    }
    if (rhandle->dents_buf)
    {
        gossip_debug(GOSSIP_DIR_DEBUG, "vfree %p\n", rhandle->dents_buf);
        vfree(rhandle->dents_buf);
        rhandle->dents_buf = NULL;
    }
    return;
}

/** Read directory entries from an instance of an open directory.
 *
 * \param filldir callback function called for each entry read.
 *
 * \retval <0 on error
 * \retval 0  when directory has been completely traversed
 * \retval >0 if we don't call filldir for all entries
 *
 * \note If the filldir call-back returns non-zero, then readdir should
 *       assume that it has had enough, and should return as well.
 */
static int pvfs2_readdir(
    struct file *file,
#ifdef HAVE_READDIR_FILE_OPERATIONS
    void *dirent,
    filldir_t filldir)
#else
    struct dir_context *ctx)
#endif
{
    int ret = 0, buffer_index;
    PVFS_ds_position *ptoken = file->private_data;
    PVFS_ds_position pos = 0;
    ino_t ino = 0;
    struct dentry *dentry = file->f_dentry;
    pvfs2_kernel_op_t *new_op = NULL;
    pvfs2_inode_t *pvfs2_inode = PVFS2_I(dentry->d_inode);
    int buffer_full = 0;
    readdir_handle_t rhandle;
    int i = 0, len = 0;
    ino_t current_ino = 0;
    char *current_entry = NULL;
    long bytes_decoded;
    char *s = kmalloc(HANDLESTRINGSIZE, GFP_KERNEL);

#ifdef HAVE_READDIR_FILE_OPERATIONS
    gossip_debug(GOSSIP_DIR_DEBUG,
        "%s: file->f_pos:%lld, ptoken = %llu\n",
        __func__,
        lld(file->f_pos),
        llu(*ptoken));
#else
    gossip_debug(GOSSIP_DIR_DEBUG,
        "%s: ctx->pos:%lld, ptoken = %llu\n",
        __func__,
        lld(ctx->pos),
        llu(*ptoken));
#endif

#ifdef HAVE_READDIR_FILE_OPERATIONS
    pos = (PVFS_ds_position)file->f_pos;
#else
    pos = (PVFS_ds_position) ctx->pos;
#endif

    /* are we done? */
    if (pos == PVFS_READDIR_END)
    {
        gossip_debug(GOSSIP_DIR_DEBUG, "%s: done\n", __func__);
        ret = 0;
        goto out;
    }

    gossip_debug(GOSSIP_DIR_DEBUG, "%s: called on %s (pos=%llu)\n",
                 __func__,
                 dentry->d_name.name,
                 llu(pos));

    rhandle.buffer_index = -1;
    rhandle.dents_buf = NULL;
    memset(&rhandle.readdir_response, 0, sizeof(rhandle.readdir_response));

    new_op = op_alloc(PVFS2_VFS_OP_READDIR);
    if (!new_op)
    {
       ret = -ENOMEM;
       goto out;
    }

    new_op->uses_shared_memory = 1;

    if (pvfs2_inode &&
       pvfs2_inode->refn.khandle.slice[0] +
         pvfs2_inode->refn.khandle.slice[3] != 0 &&
       pvfs2_inode->refn.fs_id != PVFS_FS_ID_NULL)
    {
        new_op->upcall.req.readdir.refn = pvfs2_inode->refn;
        memset(s,0,HANDLESTRINGSIZE);
        gossip_debug(GOSSIP_DIR_DEBUG,
                     "%s: upcall.req.readdir.refn.khandle:%s\n",
                     __func__,
                     k2s(&(new_op->upcall.req.readdir.refn.khandle),s));
    }
    else
    {
#if defined(HAVE_IGET5_LOCKED) || defined(HAVE_IGET4_LOCKED)
        gossip_lerr("Critical error: i_ino cannot be relied on when using iget4/5\n");
        op_release(new_op);
        ret = -EINVAL;
        goto out;
#endif
        PVFS_khandle_from(&(new_op->upcall.req.readdir.refn.khandle),
                          get_khandle_from_ino(dentry->d_inode),
                          16);
        new_op->upcall.req.readdir.refn.fs_id  = PVFS2_SB(dentry->d_inode->i_sb)->fs_id;
        memset(s,0,HANDLESTRINGSIZE);
        gossip_debug(GOSSIP_DIR_DEBUG,
                     "%s: upcall.req.readdir.refn.khandle:%s\n",
                      __func__,
                      k2s(&(new_op->upcall.req.readdir.refn.khandle),s));
    }

    new_op->upcall.req.readdir.max_dirent_count = MAX_DIRENT_COUNT_READDIR;

    /* NOTE:
     * the position we send to the readdir upcall is out of
     * sync with file->f_pos (or ctx->pos) since:
     * 1. pvfs2 doesn't include the "." and ".." entries that are added below.  
     * 2. the introduction of distributed directory logic makes token no
     *    longer be related to f_pos and pos. Instead an independent variable
     *    is used inside the function and stored in the private_data of
     *    the file structure.
     */
    new_op->upcall.req.readdir.token = *ptoken;


get_new_buffer_index:
    ret = readdir_index_get(&buffer_index);
    if (ret < 0)
    {
        gossip_lerr("pvfs2_readdir: readdir_index_get() failure (%d)\n", ret);
        op_release(new_op);
        goto out;
    }
    new_op->upcall.req.readdir.buf_index = buffer_index;

    ret = service_operation( new_op, 
                             "pvfs2_readdir", 
                             get_interruptible_flag(dentry->d_inode));

    gossip_debug(GOSSIP_DIR_DEBUG,
                 "%s: Readdir downcall status is %d.  ret:%d\n",
                  __func__,
                  new_op->downcall.status,
                  ret);

    if ( ret == -EAGAIN && op_state_purged(new_op) )
    {
       gossip_debug(GOSSIP_DIR_DEBUG,
                    "%s: Getting new buffer_index for retry of readdir.\n",
                    __func__);
       goto get_new_buffer_index;
    }

    if ( ret == -EIO && op_state_purged(new_op) )
    {
        /* pvfs2-client is down, aborting readdir.  */
        gossip_err("%s: Client is down.  Aborting readdir call. \n", __func__);
        op_release(new_op);
        goto out;
    }

    if ( ret < 0  || new_op->downcall.status != 0 )
    {
         gossip_debug(GOSSIP_DIR_DEBUG,
                      "Readdir request failed.  Status:%d\n",
                      new_op->downcall.status);
         readdir_index_put(buffer_index);
         op_release(new_op);
         ret = ((ret < 0 ? ret : new_op->downcall.status));
         goto out;
    }

    bytes_decoded = readdir_handle_ctor(&rhandle, 
                                        new_op->downcall.trailer_buf,
                                        buffer_index);
    if (bytes_decoded < 0) { 
       gossip_err("%s: could not decode trailer buffer.\n", __func__);
       ret = bytes_decoded;
       readdir_index_put(buffer_index);
       op_release(new_op);
       goto out;
    }

    if (bytes_decoded != new_op->downcall.trailer_size)
    {
        gossip_err("%s: # bytes decoded (%ld) != trailer size (%ld)\n",
                   __func__,
                   bytes_decoded,
                   (long) new_op->downcall.trailer_size);
        ret = -EINVAL;
        readdir_handle_dtor(&rhandle);
        op_release(new_op);
        goto out;
    }

    if (pos == 0)
    {
       ino = get_ino_from_khandle(dentry->d_inode);

#ifdef HAVE_READDIR_FILE_OPERATIONS
       if ( (ret=filldir(dirent, ".", 1, 0, ino, DT_DIR)) < 0)
#else
       if ( (ret=dir_emit(ctx, ".", 1, ino, DT_DIR)) < 0)
#endif
       {
          readdir_handle_dtor(&rhandle);
          op_release(new_op);
          goto out;
       }
       gossip_ldebug(GOSSIP_DIR_DEBUG,
                     "%s: dot pos:%lld\n",
                     __func__,
                     lld(pos));
       pos++;
    }

    if (pos == 1)
    {
       ino = get_parent_ino_from_dentry(dentry);

#ifdef HAVE_READDIR_FILE_OPERATIONS
       if ( (ret=filldir(dirent, "..", 2, 0, ino, DT_DIR)) < 0)
#else
       if ( (ret=dir_emit(ctx, "..", 2, ino, DT_DIR)) < 0)
#endif
       {
          readdir_handle_dtor(&rhandle);
          op_release(new_op);
          goto out;
       }
       gossip_ldebug(GOSSIP_DIR_DEBUG,
                     "%s: dot dot pos:%lld\n",
                     __func__,
                     lld(pos));
       pos++;
    }

    /*
     * we stored PVFS_ITERATE_NEXT in ctx->pos last time around
     * to prevent "finding" dot and dot-dot on any iteration
     * other than the first.
     */
#ifdef HAVE_READDIR_FILE_OPERATIONS
    if (file->f_pos == PVFS_ITERATE_NEXT) {
      file->f_pos = 0;
      pos = 0;
    }
#else
    if (ctx->pos == PVFS_ITERATE_NEXT)
      ctx->pos = 0;
#endif

     gossip_debug(GOSSIP_DIR_DEBUG,
                  "%s: dirent_outcount:%d:\n",
                   __func__,
                   rhandle.readdir_response.pvfs_dirent_outcount);

#ifdef HAVE_READDIR_FILE_OPERATIONS
    for (i = file->f_pos;
#else
    for (i = ctx->pos;
#endif
         i < rhandle.readdir_response.pvfs_dirent_outcount;
         i++)
    {
        len = rhandle.readdir_response.dirent_array[i].d_length;
        current_entry = rhandle.readdir_response.dirent_array[i].d_name;
        current_ino =
          pvfs2_khandle_to_ino(
            &(rhandle.readdir_response.dirent_array[i].khandle));

#ifdef HAVE_READDIR_FILE_OPERATIONS
        gossip_debug(GOSSIP_DIR_DEBUG, 
                    "%s: calling filldir for %s, len %d, file->f_pos:%lld:\n",
                     __func__,
                     current_entry,
                     len,
                     lld(file->f_pos));
#else
        gossip_debug(GOSSIP_DIR_DEBUG, 
                    "%s: calling dir_emit for %s, len %d, ctx->pos:%lld:\n",
                     __func__,
                     current_entry,
                     len,
                     lld(ctx->pos));
#endif
#ifdef HAVE_READDIR_FILE_OPERATIONS
	ret = filldir(dirent,
		      current_entry,
		      len,
		      file->f_pos,
		      current_ino,
		      DT_UNKNOWN);
#else
	ret = dir_emit(ctx, current_entry, len, current_ino, DT_UNKNOWN);
#endif

/*
 * The getdents buffer might fill up before the orangefs buffer.
 */
#ifdef HAVE_READDIR_FILE_OPERATIONS
        if (ret < 0) {
#else
        if (!ret) {
#endif
          gossip_debug(GOSSIP_DIR_DEBUG,
                       "%s: iterator returned:%d\n",
                       __func__,
                       ret);
          buffer_full = 1;
          break;
        }

#ifdef HAVE_READDIR_FILE_OPERATIONS
        file->f_pos++;
        pos++; 
	gossip_debug(GOSSIP_DIR_DEBUG,
                     "%s: file->f_pos:%lld:\n",
                     __func__,
                     lld(file->f_pos));
#else
        ctx->pos++;
	gossip_debug(GOSSIP_DIR_DEBUG,
                     "%s: ctx->pos:%lld:\n",
                     __func__,
                     lld(ctx->pos));
#endif
    }
    
    /*
     * we ran all the way through the last batch, set up for
     * getting another batch...
     */
    if (i == rhandle.readdir_response.pvfs_dirent_outcount) {
        *ptoken = rhandle.readdir_response.token;
#ifdef HAVE_READDIR_FILE_OPERATIONS
        file->f_pos = PVFS_ITERATE_NEXT;
#else
        ctx->pos = PVFS_ITERATE_NEXT;
#endif
    }

    /* did we hit the end of the directory? */
    if(rhandle.readdir_response.token == PVFS_READDIR_END)
    {
       gossip_debug(GOSSIP_DIR_DEBUG, "%s: trigger readdir end.\n", __func__);
#ifdef HAVE_READDIR_FILE_OPERATIONS
       file->f_pos = PVFS_READDIR_END;
#else
       ctx->pos = PVFS_READDIR_END;
#endif
       gossip_debug(GOSSIP_DIR_DEBUG, 
                    "pvfs2_readdir about to update_atime %p\n", 
                    dentry->d_inode);

       SetAtimeFlag(pvfs2_inode);
       dentry->d_inode->i_atime = CURRENT_TIME;
       mark_inode_dirty_sync(dentry->d_inode);
    }

    readdir_handle_dtor(&rhandle);
    op_release(new_op);

    gossip_debug(GOSSIP_DIR_DEBUG, "%s: returning %d\n", __func__, ret);

out:
    kfree(s);
    return (ret);
}

#ifdef HAVE_READDIRPLUS_FILE_OPERATIONS

typedef struct
{
    void *direntplus;
    uint32_t lite;
    union {
        struct {
            filldirplus_t filldirplus;
            struct kstat  ks;
        } plus;
        struct {
            unsigned long mask;
            filldirpluslite_t filldirplus_lite;
            struct kstat_lite ks;
        } plus_lite;
    } u;
} readdirplus_info_t;

typedef struct 
{
    int buffer_index;
    pvfs2_readdirplus_response_t readdirplus_response;
    void *dentsplus_buf;
} readdirplus_handle_t;

static long decode_sys_attr(char *ptr, pvfs2_readdirplus_response_t *readdirplus) 
{
    int i;
    char *buf = (char *) ptr;
    char **pptr = &buf;

    readdirplus->stat_err_array = kmalloc(readdirplus->pvfs_dirent_outcount * 
                                          sizeof(*readdirplus->stat_err_array), GFP_KERNEL);
    if (readdirplus->stat_err_array == NULL)
    {
        return -ENOMEM;
    }
    memcpy(readdirplus->stat_err_array, buf, readdirplus->pvfs_dirent_outcount * sizeof(PVFS_error));
    *pptr += readdirplus->pvfs_dirent_outcount * sizeof(PVFS_error);
    if (readdirplus->pvfs_dirent_outcount % 2) 
    {
        *pptr += 4;
    }
    readdirplus->attr_array = kmalloc(readdirplus->pvfs_dirent_outcount * 
                                      sizeof(*readdirplus->attr_array), GFP_KERNEL);
    if (readdirplus->attr_array == NULL)
    {
        return -ENOMEM;
    }
    for (i = 0; i < readdirplus->pvfs_dirent_outcount; i++)
    {
        memcpy(&readdirplus->attr_array[i], *pptr, sizeof(PVFS_sys_attr));
        *pptr += sizeof(PVFS_sys_attr);
        if (readdirplus->attr_array[i].objtype == PVFS_TYPE_SYMLINK &&
                (readdirplus->attr_array[i].mask & PVFS_ATTR_SYS_LNK_TARGET))
        {
            int *plen = NULL;
            dec_string(pptr, &readdirplus->attr_array[i].link_target, plen);
        }
        else {
            readdirplus->attr_array[i].link_target = NULL;
        }
    }
    return ((unsigned long) *pptr - (unsigned long) ptr);
}

static long decode_readdirplus_from_buffer(char *ptr, pvfs2_readdirplus_response_t *readdirplus)
{
    char *buf = ptr;
    long amt;

    amt = decode_dirents(buf, (pvfs2_readdir_response_t *) readdirplus);
    if (amt < 0)
        return amt;
    buf += amt;
    amt = decode_sys_attr(buf, readdirplus);
    if (amt < 0)
        return amt;
    buf += amt;
    return ((unsigned long) buf - (unsigned long) ptr);
}

static long readdirplus_handle_ctor(readdirplus_handle_t *rhandle, void *buf, int buffer_index)
{
    long ret;

    if (buf == NULL)
    {
        gossip_err("Invalid NULL buffer specified in readdirplus_handle_ctor\n");
        return -ENOMEM;
    }
    if (buffer_index < 0)
    {
        gossip_err("Invalid buffer index specified in readdirplus_handle_ctor\n");
        return -EINVAL;
    }
    rhandle->buffer_index = buffer_index;
    rhandle->dentsplus_buf = buf;
    if ((ret = decode_readdirplus_from_buffer(buf, &rhandle->readdirplus_response)) < 0)
    {
        gossip_err("Could not decode readdirplus from buffer %ld\n", ret);
        readdir_index_put(rhandle->buffer_index);
        rhandle->buffer_index = -1;
        gossip_debug(GOSSIP_DIR_DEBUG, "vfree %p\n", buf);
        vfree(buf);
        rhandle->dentsplus_buf = NULL;
    }
    return ret;
}

static void readdirplus_handle_dtor(readdirplus_handle_t *rhandle)
{
    if (rhandle == NULL)
    {
        return;
    }
    if (rhandle->readdirplus_response.dirent_array)
    {
        kfree(rhandle->readdirplus_response.dirent_array);
        rhandle->readdirplus_response.dirent_array = NULL;
    }
    if (rhandle->readdirplus_response.attr_array)
    {
        kfree(rhandle->readdirplus_response.attr_array);
        rhandle->readdirplus_response.attr_array = NULL;
    }
    if (rhandle->readdirplus_response.stat_err_array)
    {
        kfree(rhandle->readdirplus_response.stat_err_array);
        rhandle->readdirplus_response.stat_err_array = NULL;
    }
    if (rhandle->buffer_index >= 0)
    {
        readdir_index_put(rhandle->buffer_index);
        rhandle->buffer_index = -1;
    }
    if (rhandle->dentsplus_buf)
    {
        gossip_debug(GOSSIP_DIR_DEBUG, "vfree %p\n", rhandle->dentsplus_buf);
        vfree(rhandle->dentsplus_buf);
        rhandle->dentsplus_buf = NULL;
    }
    return;
}

static int pvfs2_readdirplus_common(
    struct file *file,
    readdirplus_info_t *info)
{
    int ret = 0, buffer_index;
    PVFS_ds_position pos = 0;
    PVFS_ds_position token = PVFS_READDIR_START;
    void *direntplus;
    ino_t ino = 0;
    struct dentry *dentry = file->f_dentry;
    pvfs2_kernel_op_t *new_op = NULL;
    pvfs2_inode_t *pvfs2_inode = PVFS2_I(dentry->d_inode);
    filldirplus_t filldirplus = NULL;
    filldirpluslite_t filldirplus_lite = NULL;
    PVFS_object_kref ref;
    int filldirplus_error = 0;
    char *s;

    direntplus = info->direntplus;
    if (info->lite == 0)
    {
        filldirplus = info->u.plus.filldirplus;
    }
    else
    {
        filldirplus_lite = info->u.plus_lite.filldirplus_lite;
    }


    pos = (PVFS_ds_position)file->f_pos;
    /* are we done? */
    if (pos == PVFS_READDIR_END)
    {
        gossip_debug(GOSSIP_DIR_DEBUG, "Skipping to graceful termination path since we are done\n");
        ret = 0;
        goto out;
    }
    gossip_debug(GOSSIP_DIR_DEBUG, "pvfs2_readdirplus called on %s (pos=%d)\n",
                 dentry->d_name.name, (int)pos);

    /* changed due to distributed directory:
     * let one call retrieve all dirent instead of multiple entrances to this function
     * !!! not tested on readdirplus.
     */
    if(pos > 2)
    {
        gossip_err("pvfs2_readdirplus: invalid pos value! \n\t no re-entrance allowed because of distributed directory structure!! \n");
        ret = -EINVAL;
        goto out;
    }

    do
    {

        switch (pos)
        {
            /*
               if we're just starting, populate the "." and ".." entries
               of the current directory; these always appear
            */
            case 0:
            {
                struct inode *inode = NULL;
                ino = get_ino_from_khandle(dentry->d_inode);
                ref.fs_id = get_fsid_from_ino(dentry->d_inode);
                PVFS_khandle_from(&refn.khandle),
                                  get_khandle_from_ino(dentry->d_inode),
                                  16);
                inode = pvfs2_iget(dentry->d_inode->i_sb, &ref);
                if (inode)
                {
                    if (info->lite == 0)
                    {
                        generic_fillattr(inode, &info->u.plus.ks);
                    }
                    else
                    {
                        generic_fillattr_lite(inode, &info->u.plus_lite.ks);
                    }
                    iput(inode);
                    gossip_debug(GOSSIP_DIR_DEBUG, "calling filldirplus of . with pos = %d\n", pos);
                    if (info->lite == 0)
                    {
                        if (filldirplus(direntplus, ".", 1, pos, ino, DT_DIR, &info->u.plus.ks) < 0)
                        {
                            filldirplus_error = 1;
                            break;
                        }
                    }
                    else 
                    {
                        if (filldirplus_lite(direntplus, ".", 1, pos, ino, DT_DIR, &info->u.plus_lite.ks) < 0)
                        {
                            filldirplus_error = 1;
                            break;
                        }
                    }
                }
                file->f_pos++;
                pos++;
                /* drop through */
            }
            case 1:
            {
                struct inode *inode = NULL;
                ino = get_parent_ino_from_dentry(dentry);
                ref.fs_id = get_fsid_from_ino(dentry->d_parent->d_inode);
                PVFS_khandle_from(&refn.khandle,
                             get_khandle_from_ino(dentry->d_parent->d_inode),
                             16);
                inode = pvfs2_iget(dentry->d_inode->i_sb, &ref);
                if (inode) 
                {
                    if (info->lite == 0)
                    {
                        generic_fillattr(inode, &info->u.plus.ks);
                    }
                    else
                    {
                        generic_fillattr_lite(inode, &info->u.plus_lite.ks);
                    }
                    iput(inode);
                    gossip_debug(GOSSIP_DIR_DEBUG, "calling filldirplus of .. with pos = %d\n", pos);
                    if (info->lite == 0)
                    {
                        if (filldirplus(direntplus, "..", 2, pos, ino, DT_DIR, &info->u.plus.ks) < 0)
                        {
                            filldirplus_error = 1;
                            break;
                        }
                    }
                    else
                    {
                        if (filldirplus_lite(direntplus, "..", 2, pos, ino, DT_DIR, &info->u.plus_lite.ks) < 0)
                        {
                            filldirplus_error = 1;
                            break;
                        }
                    }
                }
                file->f_pos++;
                pos++;
                /* drop through */
            }
            default:
            {
                readdirplus_handle_t rhandle;
                uint32_t pvfs2_mask;

                rhandle.buffer_index = -1;
                rhandle.dentsplus_buf = NULL;
                memset(&rhandle.readdirplus_response, 0, sizeof(rhandle.readdirplus_response));
                pvfs2_mask = (info->lite == 0) ? PVFS_ATTR_SYS_ALL : convert_to_pvfs2_mask(info->u.plus_lite.mask);

                /* handle the normal cases here */
                new_op = op_alloc(PVFS2_VFS_OP_READDIRPLUS);
                if (!new_op)
                {
                    return -ENOMEM;
                }
                if (pvfs2_inode &&
                    pvfs2_inode->refn.khandle.slice[0] +
                      pvfs2_inode->refn.khandle.slice[0] != 0 &&
                    pvfs2_inode->refn.fs_id != PVFS_FS_ID_NULL)
                {
                    new_op->upcall.req.readdirplus.refn = pvfs2_inode->refn;
                }
                else
                {
#if defined(HAVE_IGET5_LOCKED) || defined(HAVE_IGET4_LOCKED)
                    gossip_lerr("Critical error: i_ino cannot be relied on "
                                "when using iget4/5\n");
                    op_release(new_op);
                    return -EINVAL;
#endif
                    PVFS_khandle_from(
                      &(new_op->upcall.req.readdirplus.refn.khandle),
                      get_khandle_from_ino(dentry->d_inode),
                      16);
                    new_op->upcall.req.readdirplus.refn.fs_id =
                        PVFS2_SB(dentry->d_inode->i_sb)->fs_id;
                }
                new_op->upcall.req.readdirplus.mask = pvfs2_mask;
                new_op->upcall.req.readdirplus.max_dirent_count 
                        = MAX_DIRENT_COUNT_READDIRPLUS;

                /* NOTE:
                   the position we send to the readdir upcall is out of
                   sync with file->f_pos since
                   1.  pvfs2 doesn't include the "." and ".." entries that
                   we added above.
                   2. the introduction of distributed directory structure make
                   token not related to f_pos and pos anymore.

                   So an independent variable is used inside the function.

                */
                new_op->upcall.req.readdirplus.token = token;

                ret = readdir_index_get(&buffer_index);
                if (ret < 0)
                {
                    gossip_err("pvfs2_readdirplus: readdir_index_get() "
                               "failure (%d)\n", ret);
                    goto err;
                }
                new_op->upcall.req.readdirplus.buf_index = buffer_index;

                ret = service_operation(
                    new_op, "pvfs2_readdirplus", 
                    get_interruptible_flag(dentry->d_inode));

                gossip_debug(GOSSIP_DIR_DEBUG, "Readdirplus downcall status is %d\n",
                        new_op->downcall.status);

                if (new_op->downcall.status == 0)
                {
                    int i = 0, len = 0;
                    ino_t current_ino = 0;
                    char *current_entry = NULL;
                    long bytes_decoded;

                    ret = 0;
                    if ((bytes_decoded = readdirplus_handle_ctor(&rhandle,
                                    new_op->downcall.trailer_buf, buffer_index)) < 0)
                    {
                        ret = bytes_decoded;
                        gossip_err("pvfs2_readdirplus: Could not decode trailer buffer "
                                " into a readdirplus response %d\n", ret);
                        goto err;
                    }
                    if (bytes_decoded != new_op->downcall.trailer_size)
                    {
                        gossip_err("pvfs2_readdirplus: # bytes decoded (%ld) != trailer size (%ld)\n",
                                bytes_decoded, (long) new_op->downcall.trailer_size);
                        ret = -EINVAL;
                        goto err;
                    }

                    if (rhandle.readdirplus_response.pvfs_dirent_outcount == 0)
                    {
                        goto graceful_termination_path;
                    }

                    for (i = 0; i < rhandle.readdirplus_response.pvfs_dirent_outcount; i++)
                    {
                        struct inode *filled_inode = NULL;
                        int dt_type, stat_error;
                        void *ptr = NULL;
                        PVFS_sys_attr *attrs = NULL;
                        PVFS_khandle khandle;
                        PVFS_fs_id fs_id;

                        len = rhandle.readdirplus_response.dirent_array[i].d_length;
                        current_entry = rhandle.readdirplus_response.dirent_array[i].d_name;
                        khandle = rhandle.readdirplus_response.dirent_array[i].khandle;
                        current_ino = pvfs2_khandle_to_ino(khandle);
                        stat_error = rhandle.readdirplus_response.stat_err_array[i];
                        fs_id  = new_op->upcall.req.readdirplus.refn.fs_id;

                        if (stat_error == 0)
                        {
                            ref.fs_id = get_fsid_from_ino(dentry->d_inode);
                            ref.khandle = khandle;
                            /* locate inode in the icache, but don't getattr() */
                            filled_inode = pvfs2_iget_locked(dentry->d_inode->i_sb, &ref);
                            if (filled_inode == NULL) {
                                gossip_err("Could not allocate inode\n");
                                ret = -ENOMEM;
                                goto err;
                            }
                            else if (is_bad_inode(filled_inode)) {
                                iput(filled_inode);
                                gossip_err("bad inode obtained from iget_locked\n");
                                ret = -EINVAL;
                                goto err;
                            }
                            else {
                                attrs = &rhandle.readdirplus_response.attr_array[i];
                                if ((ret = copy_attributes_to_inode(filled_inode, attrs, attrs->link_target)) < 0) {
                                    gossip_err("copy attributes to inode failed with err %d\n", ret);
                                    iput(filled_inode);
                                    goto err;
                                }
                                if (info->lite == 0)
                                {
                                    generic_fillattr(filled_inode, &info->u.plus.ks);
                                }
                                else
                                {
                                    generic_fillattr_lite(filled_inode, &info->u.plus_lite.ks);
                                }
                                if (filled_inode->i_state & I_NEW) {
                                    pvfs2_inode_t *filled_pvfs2_inode = PVFS2_I(filled_inode);
                                    pvfs2_inode_initialize(filled_pvfs2_inode);
                                    filled_pvfs2_inode->refn.khandle = khandle;
                                    filled_pvfs2_inode->refn.fs_id  = fs_id;
                                    filled_inode->i_mapping->host = filled_inode;
                                    filled_inode->i_rdev = 0;
                                    filled_inode->i_bdev = NULL;
                                    filled_inode->i_cdev = NULL;
                                    filled_inode->i_mapping->a_ops = &pvfs2_address_operations;
                                    filled_inode->i_mapping->backing_dev_info = &pvfs2_backing_dev_info;
                                    /* Make sure that we unlock the inode */
                                    unlock_new_inode(filled_inode);
                                }
                                iput(filled_inode);
                            }
                            if (info->lite == 0)
                            {
                                ptr = &info->u.plus.ks;
                            }
                            else
                            {
                                ptr = &info->u.plus_lite.ks;
                            }
                            if (attrs->objtype == PVFS_TYPE_METAFILE) 
                            {
                                dt_type = DT_REG;
                            }
                            else if (attrs->objtype == PVFS_TYPE_DIRECTORY) 
                            {
                                dt_type = DT_DIR;
                            }
                            else if (attrs->objtype == PVFS_TYPE_SYMLINK) 
                            {
                                dt_type = DT_LNK;
                            }
                            else 
                            {
                                dt_type = DT_UNKNOWN;
                            }
                        }
                        else {
                            int err_num = pvfs2_normalize_to_errno(stat_error);
                            ptr = ERR_PTR(err_num);
                            dt_type = DT_UNKNOWN;
                        }
                        s = kzalloc(HANDLESTRINGSIZE, GFP_KERNEL);
                        gossip_debug(GOSSIP_DIR_DEBUG,
                                     "calling filldirplus for %s "
                                     " (%s) with len %d, pos %ld kstat %p\n", 
                                     current_entry,
                                     k2s(&khandle,s),
                                     len,
                                     (unsigned long) pos,
                                     ptr);
                        kfree(s);
                    
                        if (info->lite == 0)
                        {
                            ret = filldirplus(direntplus, current_entry, len, pos,
                                    current_ino, dt_type, ptr);
                        }
                        else 
                        {
                            ret = filldirplus_lite(direntplus, current_entry, len, pos,
                                    current_ino, dt_type, ptr);
                        }
                        /* filldirplus has had enough */
                        if (ret < 0)
                        {
                            /* in this case, the readdir will just fail with
                               -EINVAL because reentering readdirplus will generate
                               a non-valid pos value, issue a warning here. */
                            filldirplus_error = 1;
                            gossip_err("WARNING: filldirplus failed with err %d, will probably causing readdirplus to fail with -EINVAL error!!!\n", ret);
                            ret = 0;
                            break;
                        }
                        file->f_pos++;
                        pos++;
                    }

                    /* update token and pos */
                    if (i == rhandle.readdirplus_response.pvfs_dirent_outcount)
                    {
                        token = rhandle.readdirplus_response.token;
                    }
                    else
                    {
                        pos -= (i - 1);
                        file->f_pos = (i - 1);
                        gossip_debug(GOSSIP_DIR_DEBUG, "at least one filldir call failed. Not updating token. Setting f_pos to: %ld\n", (unsigned long) file->f_pos);
                    }

                    /* did we hit the end of the directory? */
                    if(token == PVFS_READDIR_END && !filldirplus_error)
                    {
                        gossip_debug(GOSSIP_DIR_DEBUG,
                            "End of dir detected; setting f_pos to PVFS_READDIR_END.\n");
                        file->f_pos = PVFS_READDIR_END;
                    }

                    gossip_debug(GOSSIP_DIR_DEBUG,
                        "pos = %d, token = %llu, file->f_pos is %ld\n", 
                        pos, llu(token), (unsigned long) file->f_pos);
                }
                else
                {
                    readdir_index_put(buffer_index);
                    gossip_debug(GOSSIP_DIR_DEBUG, "Failed to readdirplus (downcall status %d)\n",
                                new_op->downcall.status);
                }
err:
                readdirplus_handle_dtor(&rhandle);
                op_release(new_op);
                break;
            }
        }
    }
    while(!filldirplus_error && (ret >= 0) && (file->f_pos != PVFS_READDIR_END)

    if (ret == 0)
    {
        SetAtimeFlag(pvfs2_inode);
        dentry->d_inode->i_atime = CURRENT_TIME;
        mark_inode_dirty_sync(dentry->d_inode);
    }

    gossip_debug(GOSSIP_DIR_DEBUG, "pvfs2_readdirplus returning %d\n",ret);
    return ret;
}

/** Read directory entries from an instance of an open directory 
 *  and the associated attributes for every entry in one-shot.
 *
 * \param filldirplus callback function called for each entry read.
 *
 * \retval <0 on error
 * \retval 0  when directory has been completely traversed
 * \retval >0 if we don't call filldir for all entries
 *
 * \note If the filldirplus call-back returns non-zero, then readdirplus should
 *       assume that it has had enough, and should return as well.
 */
static int pvfs2_readdirplus(
    struct file *file,
    void *direntplus,
    filldirplus_t filldirplus)
{
    readdirplus_info_t info;

    memset(&info, 0, sizeof(info));
    info.direntplus = direntplus;
    info.lite = 0;
    info.u.plus.filldirplus = filldirplus;
    return pvfs2_readdirplus_common(file, &info);
}

/** Read directory entries from an instance of an open directory 
 *  and the associated attributes for every entry in one-shot.
 *
 * \param filldirplus_lite callback function called for each entry read.
 *
 * \retval <0 on error
 * \retval 0  when directory has been completely traversed
 * \retval >0 if we don't call filldir for all entries
 *
 * \note If the filldirplus_lite call-back returns non-zero, then readdirplus_lite should
 *       assume that it has had enough, and should return as well.
 *  The only difference is that stat information is not returned uptodate!
 */
static int pvfs2_readdirplus_lite(
    struct file *file,
    unsigned long lite_mask,
    void *direntplus_lite,
    filldirpluslite_t filldirplus_lite)
{
    readdirplus_info_t info;

    memset(&info, 0, sizeof(info));
    info.direntplus = direntplus_lite;
    info.lite = 1;
    info.u.plus_lite.mask = lite_mask;
    info.u.plus_lite.filldirplus_lite = filldirplus_lite;
    info.u.plus_lite.ks.lite_mask = lite_mask;
    return pvfs2_readdirplus_common(file, &info);
}
#endif

static int pvfs2_dir_open(struct inode *inode, struct file *file)
{
        __u64 *ptoken;

        gossip_debug(GOSSIP_DIR_DEBUG,
                     "%s: called on %s\n",
                     __func__,
                     file->f_dentry->d_name.name);

        file->private_data = kmalloc(sizeof(__u64), GFP_KERNEL);
        if (!file->private_data)
                return -ENOMEM;

        ptoken = file->private_data;
        *ptoken = PVFS_READDIR_START;
        return 0;
}

static int pvfs2_dir_release(struct inode *inode, struct file *file)
{
        gossip_debug(GOSSIP_DIR_DEBUG,
                     "%s: called on %s\n",
                     __func__,
                     file->f_dentry->d_name.name);

        pvfs2_flush_inode(inode);
        kfree(file->private_data);
        return 0;
}

/** PVFS2 implementation of VFS directory operations */
const struct file_operations pvfs2_dir_operations =
{
#ifdef PVFS2_LINUX_KERNEL_2_4
    read : generic_read_dir,
    readdir : pvfs2_readdir,
    open : pvfs2_file_open,
    release : pvfs2_file_release,
#else
    .read = generic_read_dir,
#ifdef HAVE_READDIR_FILE_OPERATIONS
    .readdir = pvfs2_readdir,
#else
    .iterate = pvfs2_readdir,
#endif
#ifdef HAVE_READDIRPLUS_FILE_OPERATIONS
    .readdirplus = pvfs2_readdirplus,
#endif
#ifdef HAVE_READDIRPLUSLITE_FILE_OPERATIONS
    .readdirplus_lite = pvfs2_readdirplus_lite,
#endif
    .open = pvfs2_dir_open,
    .release = pvfs2_dir_release,
#endif
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */

