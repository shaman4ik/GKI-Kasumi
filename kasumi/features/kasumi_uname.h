/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * Kasumi - interfaces for global and scoped uname spoofing.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#ifndef _KASUMI_UNAME_H
#define _KASUMI_UNAME_H

#include <linux/types.h>
#include "kasumi_uapi.h"

struct task_struct;

/* Resolve required kernel symbols. Call once at module init. Returns 0 on success. */
int kasumi_uname_init(void);

/* True when the module can perform at least global-mode spoofing (init_uts_ns+uts_sem). */
bool kasumi_uname_capable(void);

/* Module teardown: best-effort restore init_uts_ns if global mode was active. */
void kasumi_uname_exit(void);

/* ----- Global mode ------------------------------------------------------ */

/*
 * Apply spoof to init_uts_ns (and therefore every task using it).
 * Fields with a non-empty first byte override the originals; empty fields
 * are left untouched. Originals are saved on first call for later restore.
 */
int kasumi_uname_apply_global(const struct kasumi_spoof_uname *u);

/* Restore init_uts_ns to the originals captured on first apply_global. */
int kasumi_uname_restore_global(void);

bool kasumi_uname_global_active(void);

/* ----- Scoped (per-task uts_ns) ---------------------------------------- */

/* Store spoof config for scoped mode. NULL disables. */
int kasumi_uname_set_scoped_config(const struct kasumi_spoof_uname *u);

bool kasumi_uname_scoped_active(void);

/*
 * Called from per-syscall fast path for tasks that should see spoofed uname.
 * Idempotent: if current already owns a private uts_ns, returns immediately.
 * On first call it unshares the current task's uts_ns (CLONE_NEWUTS) and
 * writes the spoofed fields into the new copy. After that, every reader of
 * utsname() in this task (and its children) sees the fake values — uname(2),
 * /proc/version, /proc/sys/kernel/{ostype,osrelease,version,domainname},
 * and any other in-kernel consumer.
 */
void kasumi_uname_apply_scoped_current(void);

#endif /* _KASUMI_UNAME_H */
