/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * Kasumi - super_operations shadow overrides for per-inode hook cleanup.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#ifndef _KASUMI_SOP_OVERRIDE_H
#define _KASUMI_SOP_OVERRIDE_H

#include <linux/fs.h>

int kasumi_sop_override_init(void);
void kasumi_sop_override_exit(void);
int kasumi_sop_install(struct super_block *sb);

#endif /* _KASUMI_SOP_OVERRIDE_H */
