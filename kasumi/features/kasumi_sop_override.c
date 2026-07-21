/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * Kasumi - super_operations shadow installation for trap-free inode cleanup.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#include "kasumi_fop_override.h"
#include "kasumi_iop_override.h"
#include "kasumi_runtime.h"
#include "kasumi_sop_override.h"

#include <linux/hashtable.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define KASUMI_SOP_HASH_BITS 8

struct kasumi_sop_meta {
	struct super_block *sb;
	const struct super_operations *orig_sop;
	struct super_operations shadow_sop;
	struct hlist_node node;
	struct rcu_head rcu;
};

static DEFINE_HASHTABLE(kasumi_sop_table, KASUMI_SOP_HASH_BITS);
static DEFINE_SPINLOCK(kasumi_sop_lock);
static bool kasumi_sop_ready;

static struct kasumi_sop_meta *kasumi_sop_lookup_rcu(struct super_block *sb)
{
	struct kasumi_sop_meta *m;

	hash_for_each_possible_rcu(kasumi_sop_table, m, node, (unsigned long)sb) {
		if (m->sb == sb)
			return m;
	}
	return NULL;
}

static void kasumi_sop_meta_free_rcu(struct rcu_head *rcu)
{
	struct kasumi_sop_meta *m = container_of(rcu, struct kasumi_sop_meta, rcu);

	kfree(m);
}

static void kasumi_shadow_destroy_inode(struct inode *inode)
{
	struct kasumi_sop_meta *m;
	const struct super_operations *orig = NULL;

	atomic64_inc(&kasumi_hook_stats.sop_destroy_inode);
	kasumi_iop_cleanup_inode(inode);
	kasumi_fop_cleanup_inode(inode);

	if (!inode || !inode->i_sb)
		return;

	rcu_read_lock();
	m = kasumi_sop_lookup_rcu(inode->i_sb);
	if (m)
		orig = m->orig_sop;
	rcu_read_unlock();

	if (orig && orig->destroy_inode)
		orig->destroy_inode(inode);
}

int kasumi_sop_install(struct super_block *sb)
{
	struct kasumi_sop_meta *m, *existing;
	const struct super_operations *orig;

	if (!READ_ONCE(kasumi_sop_ready))
		return -EOPNOTSUPP;
	if (!sb || !sb->s_op)
		return -EINVAL;

	orig = READ_ONCE(sb->s_op);

	m = kzalloc(sizeof(*m), GFP_ATOMIC);
	if (!m)
		return -ENOMEM;

	m->sb = sb;
	m->orig_sop = orig;
	memcpy(&m->shadow_sop, orig, sizeof(struct super_operations));
	m->shadow_sop.destroy_inode = kasumi_shadow_destroy_inode;
	if (!orig->destroy_inode && !orig->free_inode) {
		if (!kasumi_free_inode_nonrcu_ptr) {
			kfree(m);
			return -EOPNOTSUPP;
		}
		m->shadow_sop.free_inode = kasumi_free_inode_nonrcu_ptr;
	}

	spin_lock(&kasumi_sop_lock);
	existing = kasumi_sop_lookup_rcu(sb);
	if (existing) {
		spin_unlock(&kasumi_sop_lock);
		kfree(m);
		return 0;
	}
	if (READ_ONCE(sb->s_op) != orig) {
		spin_unlock(&kasumi_sop_lock);
		kfree(m);
		return -EAGAIN;
	}

	hash_add_rcu(kasumi_sop_table, &m->node, (unsigned long)sb);
	smp_wmb();
	WRITE_ONCE(sb->s_op, &m->shadow_sop);
	spin_unlock(&kasumi_sop_lock);

	kasumi_log("sop_override: installed on sb %p (orig=%p)\n", sb, orig);
	return 0;
}

int kasumi_sop_override_init(void)
{
	hash_init(kasumi_sop_table);
	WRITE_ONCE(kasumi_sop_ready, true);
	pr_info("Kasumi: sop_override initialized\n");
	return 0;
}

void kasumi_sop_override_exit(void)
{
	struct kasumi_sop_meta *m;
	struct hlist_node *tmp;
	int bkt;

	WRITE_ONCE(kasumi_sop_ready, false);

	spin_lock(&kasumi_sop_lock);
	hash_for_each_safe(kasumi_sop_table, bkt, tmp, m, node) {
		if (m->sb && m->sb->s_op == &m->shadow_sop)
			WRITE_ONCE(m->sb->s_op, m->orig_sop);
		hash_del_rcu(&m->node);
		call_rcu(&m->rcu, kasumi_sop_meta_free_rcu);
	}
	spin_unlock(&kasumi_sop_lock);

	rcu_barrier();
	pr_info("Kasumi: sop_override exited\n");
}
