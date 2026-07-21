/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * Kasumi - lookup-time inode_operations shadow installation for fast getattr spoofing.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#include "kasumi_entrypoints.h"
#include "kasumi_iop_override.h"
#include "kasumi_runtime.h"
#include "kasumi_sop_override.h"

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/version.h>
#include <linux/namei.h>

#define KASUMI_IOP_HASH_BITS 10

struct kasumi_iop_meta {
	struct inode *inode;                          /* hash key */
	const struct inode_operations *orig_iop;      /* what i_op pointed to before */
	struct inode_operations shadow_iop;           /* our copy with .getattr patched */
	struct hlist_node node;
	struct rcu_head rcu;
};

static DEFINE_HASHTABLE(kasumi_iop_table, KASUMI_IOP_HASH_BITS);
static DEFINE_SPINLOCK(kasumi_iop_lock);

/* ------------------------------------------------------------------ */
/* hash table helpers                                                  */
/* ------------------------------------------------------------------ */

static struct kasumi_iop_meta *kasumi_iop_lookup_rcu(struct inode *inode)
{
	struct kasumi_iop_meta *m;

	hash_for_each_possible_rcu(kasumi_iop_table, m, node, (unsigned long)inode) {
		if (m->inode == inode)
			return m;
	}
	return NULL;
}

static void kasumi_iop_meta_free_rcu(struct rcu_head *rcu)
{
	struct kasumi_iop_meta *m = container_of(rcu, struct kasumi_iop_meta, rcu);
	kfree(m);
}

/* ------------------------------------------------------------------ */
/* shadow getattr - signature varies across kernel versions             */
/* ------------------------------------------------------------------ */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0))
KASUMI_NOCFI static int kasumi_shadow_getattr(struct mnt_idmap *idmap,
					  const struct path *path,
					  struct kstat *stat,
					  u32 request_mask,
					  unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct kasumi_iop_meta *m;
	const struct inode_operations *orig = NULL;
	int ret;

	atomic64_inc(&kasumi_hook_stats.iop_getattr_entries);
	rcu_read_lock();
	m = kasumi_iop_lookup_rcu(inode);
	if (m)
		orig = m->orig_iop;
	rcu_read_unlock();

	if (orig && orig->getattr)
		ret = orig->getattr(idmap, path, stat, request_mask, query_flags);
	else {
		generic_fillattr(idmap, request_mask, inode, stat);
		ret = 0;
	}

	if (ret == 0 && inode && inode->i_mapping &&
	    test_bit(AS_FLAGS_KASUMI_SPOOF_KSTAT, &inode->i_mapping->flags)) {
		kasumi_apply_kstat_spoof(inode, stat);
		atomic64_inc(&kasumi_hook_stats.iop_getattr_spoofs);
	}
	return ret;
}
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0))
KASUMI_NOCFI static int kasumi_shadow_getattr(struct user_namespace *userns,
					  const struct path *path,
					  struct kstat *stat,
					  u32 request_mask,
					  unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct kasumi_iop_meta *m;
	const struct inode_operations *orig = NULL;
	int ret;

	atomic64_inc(&kasumi_hook_stats.iop_getattr_entries);
	rcu_read_lock();
	m = kasumi_iop_lookup_rcu(inode);
	if (m)
		orig = m->orig_iop;
	rcu_read_unlock();

	if (orig && orig->getattr)
		ret = orig->getattr(userns, path, stat, request_mask, query_flags);
	else {
		generic_fillattr(userns, inode, stat);
		ret = 0;
	}

	if (ret == 0 && inode && inode->i_mapping &&
	    test_bit(AS_FLAGS_KASUMI_SPOOF_KSTAT, &inode->i_mapping->flags)) {
		kasumi_apply_kstat_spoof(inode, stat);
		atomic64_inc(&kasumi_hook_stats.iop_getattr_spoofs);
	}
	return ret;
}
#else
KASUMI_NOCFI static int kasumi_shadow_getattr(const struct path *path,
					  struct kstat *stat,
					  u32 request_mask,
					  unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct kasumi_iop_meta *m;
	const struct inode_operations *orig = NULL;
	int ret;

	atomic64_inc(&kasumi_hook_stats.iop_getattr_entries);
	rcu_read_lock();
	m = kasumi_iop_lookup_rcu(inode);
	if (m)
		orig = m->orig_iop;
	rcu_read_unlock();

	if (orig && orig->getattr)
		ret = orig->getattr(path, stat, request_mask, query_flags);
	else {
		generic_fillattr(inode, stat);
		ret = 0;
	}

	if (ret == 0 && inode && inode->i_mapping &&
	    test_bit(AS_FLAGS_KASUMI_SPOOF_KSTAT, &inode->i_mapping->flags)) {
		kasumi_apply_kstat_spoof(inode, stat);
		atomic64_inc(&kasumi_hook_stats.iop_getattr_spoofs);
	}
	return ret;
}
#endif

/* ------------------------------------------------------------------ */
/* install / uninstall                                                  */
/* ------------------------------------------------------------------ */

int kasumi_iop_install(struct inode *inode)
{
	struct kasumi_iop_meta *m, *existing;
	const struct inode_operations *orig;

	if (!inode || !inode->i_op)
		return -EINVAL;
	if (!inode->i_mapping)
		return -EINVAL;
	if (!inode->i_sb)
		return -EINVAL;

	if (test_bit(AS_FLAGS_KASUMI_IOP_INSTALLED, &inode->i_mapping->flags))
		return 0;
	if (kasumi_sop_install(inode->i_sb))
		return -EOPNOTSUPP;

	orig = inode->i_op;

	m = kzalloc(sizeof(*m), GFP_ATOMIC);
	if (!m)
		return -ENOMEM;

	m->inode = inode;
	m->orig_iop = orig;
	memcpy(&m->shadow_iop, orig, sizeof(struct inode_operations));
	m->shadow_iop.getattr = kasumi_shadow_getattr;

	spin_lock(&kasumi_iop_lock);
	/* Race: another CPU may have installed concurrently. */
	existing = kasumi_iop_lookup_rcu(inode);
	if (existing) {
		spin_unlock(&kasumi_iop_lock);
		kfree(m);
		return 0;
	}
	hash_add_rcu(kasumi_iop_table, &m->node, (unsigned long)inode);
	/* Publish: set flag THEN swap pointer so readers seeing new i_op
	 * also see the flag. */
	set_bit(AS_FLAGS_KASUMI_IOP_INSTALLED, &inode->i_mapping->flags);
	smp_wmb();
	WRITE_ONCE(inode->i_op, &m->shadow_iop);
	spin_unlock(&kasumi_iop_lock);

	kasumi_log("iop_override: installed on inode %p (orig=%p)\n", inode, orig);
	return 0;
}

int kasumi_iop_mark_spoof(struct inode *inode)
{
	if (!inode || !inode->i_mapping)
		return -EINVAL;
	set_bit(AS_FLAGS_KASUMI_SPOOF_KSTAT, &inode->i_mapping->flags);
	return kasumi_iop_install(inode);
}

/*
 * Internal uninstall (called from super_operations destroy_inode and from exit).
 * Restores original i_op pointer and frees metadata after RCU grace period.
 * Safe to call when not installed.
 */
static void kasumi_iop_uninstall_locked(struct inode *inode)
{
	struct kasumi_iop_meta *m;

	m = kasumi_iop_lookup_rcu(inode);
	if (!m)
		return;

	/* Restore original first so any in-flight reader either sees old or
	 * shadow (both valid until RCU grace ends). */
	if (inode->i_op == &m->shadow_iop)
		WRITE_ONCE(inode->i_op, m->orig_iop);

	hash_del_rcu(&m->node);
	if (inode->i_mapping)
		clear_bit(AS_FLAGS_KASUMI_IOP_INSTALLED, &inode->i_mapping->flags);

	call_rcu(&m->rcu, kasumi_iop_meta_free_rcu);
}

void kasumi_iop_cleanup_inode(struct inode *inode)
{
	if (!inode)
		return;

	spin_lock(&kasumi_iop_lock);
	kasumi_iop_uninstall_locked(inode);
	spin_unlock(&kasumi_iop_lock);
}

/* ------------------------------------------------------------------ */
/* init / exit                                                          */
/* ------------------------------------------------------------------ */

int kasumi_iop_override_init(void)
{
	hash_init(kasumi_iop_table);
	pr_info("Kasumi: iop_override initialized (cleanup via sop_override)\n");
	return 0;
}

void kasumi_iop_override_exit(void)
{
	struct kasumi_iop_meta *m;
	struct hlist_node *tmp;
	int bkt;

	/*
	 * Restore all live shadow installs. Module exit has disabled runtime
	 * entry points before this runs, so no new entries should be added.
	 */
	spin_lock(&kasumi_iop_lock);
	hash_for_each_safe(kasumi_iop_table, bkt, tmp, m, node) {
		if (m->inode && m->inode->i_op == &m->shadow_iop)
			WRITE_ONCE(m->inode->i_op, m->orig_iop);
		if (m->inode && m->inode->i_mapping)
			clear_bit(AS_FLAGS_KASUMI_IOP_INSTALLED,
				  &m->inode->i_mapping->flags);
		hash_del_rcu(&m->node);
		call_rcu(&m->rcu, kasumi_iop_meta_free_rcu);
	}
	spin_unlock(&kasumi_iop_lock);

	/* Wait for outstanding RCU readers and freelist callbacks. */
	rcu_barrier();
	pr_info("Kasumi: iop_override exited\n");
}
