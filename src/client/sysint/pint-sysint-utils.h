/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* internal helper functions used by the system interface */

#ifndef __PINT_SYSINT_UTILS_H
#define __PINT_SYSINT_UTILS_H

#include <stdio.h>
#include <string.h>
#ifdef WIN32
#include "wincommon.h"
#else
#include <sys/time.h>
#endif
#include <errno.h>
#include <assert.h>

#include "pvfs2-types.h"
#include "pvfs2-attr.h"
#include "gossip.h"
#include "job.h"
#include "bmi.h"
#include "pvfs2-sysint.h"
#include "gen-locks.h"
#include "pint-cached-config.h"
#include "pint-perf-counter.h"
#include "pvfs2-sysint.h"

#include "trove.h"
#include "client-state-machine.h"
#include "server-config.h"

int PINT_server_get_config(struct server_configuration_s *config,
                           struct PVFS_sys_mntent* mntent_p,
                           const PVFS_credential *credential,
                           PVFS_hint hints);

struct server_configuration_s *PINT_get_server_config_struct(PVFS_fs_id fs_id);

void PINT_put_server_config_struct(struct server_configuration_s *config);

int PINT_lookup_parent(char *filename,
                       PVFS_fs_id fs_id,
                       PVFS_credential *credential,
                       PVFS_handle * handle);

int PINT_client_security_initialize(void);
int PINT_client_security_finalize(void);

/* client only function to start update timer for perf counted */
int client_perf_start_rollover(struct PINT_perf_counter *pc,
                               struct PINT_perf_counter *tpc);

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */

#endif
