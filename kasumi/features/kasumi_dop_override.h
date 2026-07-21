/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * Kasumi - dentry_operations shadow overrides for d_path presentation.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#ifndef _KASUMI_DOP_OVERRIDE_H
#define _KASUMI_DOP_OVERRIDE_H

#include <linux/dcache.h>

int kasumi_dop_override_init(void);
void kasumi_dop_override_exit(void);
int kasumi_dop_install(struct dentry *dentry, const char *display_path);
int kasumi_dop_uninstall_path(const char *path);

#endif /* _KASUMI_DOP_OVERRIDE_H */
