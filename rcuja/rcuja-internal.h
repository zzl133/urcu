#ifndef _URCU_RCUJA_INTERNAL_H
#define _URCU_RCUJA_INTERNAL_H

/*
 * rcuja/rcuja-internal.h
 *
 * Userspace RCU library - RCU Judy Array Internal Header
 *
 * Copyright 2012 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>
#include <urcu/rculfhash.h>

/* Never declared. Opaque type used to store flagged node pointers. */
struct cds_ja_inode_flag;

/*
 * Shadow node contains mutex and call_rcu head associated with a node.
 */
struct cds_ja_shadow_node {
	struct cds_lfht_node ht_node;	/* hash table node */
	struct cds_ja_inode *node;	/* reverse mapping and hash table key */
	/*
	 * mutual exclusion on all nodes belonging to the same tree
	 * position (e.g. both nodes before and after recompaction
	 * use the same lock).
	 */
	pthread_mutex_t *lock;
	unsigned int nr_child;		/* number of children in node */
	struct rcu_head head;		/* for deferred node and shadow node reclaim */
	int is_root;			/* is it a root node ? */
	int fallback_removal_count;	/* removals left keeping fallback */
};

struct cds_ja {
	struct cds_ja_inode_flag *root;
	unsigned int tree_depth;
	uint64_t key_max;
	/*
	 * We use a hash table to associate node keys to their
	 * respective shadow node. This helps reducing lookup hot path
	 * cache footprint, especially for very small nodes.
	 */
	struct cds_lfht *ht;
	unsigned long nr_fallback;	/* Number of fallback nodes used */
};

__attribute__((visibility("protected")))
struct cds_ja_shadow_node *rcuja_shadow_lookup_lock(struct cds_lfht *ht,
		struct cds_ja_inode *node);

__attribute__((visibility("protected")))
void rcuja_shadow_unlock(struct cds_ja_shadow_node *shadow_node);

__attribute__((visibility("protected")))
struct cds_ja_shadow_node *rcuja_shadow_set(struct cds_lfht *ht,
		struct cds_ja_inode *new_node,
		struct cds_ja_shadow_node *inherit_from);

/* rcuja_shadow_clear flags */
enum {
	RCUJA_SHADOW_CLEAR_FREE_NODE = (1U << 0),
	RCUJA_SHADOW_CLEAR_FREE_LOCK = (1U << 1),
};

__attribute__((visibility("protected")))
int rcuja_shadow_clear(struct cds_lfht *ht,
		struct cds_ja_inode *node,
		struct cds_ja_shadow_node *shadow_node,
		unsigned int flags);

__attribute__((visibility("protected")))
void rcuja_shadow_prune(struct cds_lfht *ht,
		unsigned int flags);

__attribute__((visibility("protected")))
struct cds_lfht *rcuja_create_ht(const struct rcu_flavor_struct *flavor);

__attribute__((visibility("protected")))
int rcuja_delete_ht(struct cds_lfht *ht);

#define DEBUG

#ifdef DEBUG
#define dbg_printf(fmt, args...)     printf("[debug rcuja] " fmt, ## args)
#else
#define dbg_printf(fmt, args...)				\
do {								\
	/* do nothing but check printf format */		\
	if (0)							\
		printf("[debug rcuja] " fmt, ## args);	\
} while (0)
#endif

#endif /* _URCU_RCUJA_INTERNAL_H */
