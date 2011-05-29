/*
 * urcu-rbtree.c
 *
 * Userspace RCU library - Red-Black Tree
 *
 * Copyright (c) 2010 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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
 *
 * Implementation of RCU-adapted data structures and operations based on the RB
 * tree algorithms found in chapter 12 of:
 *
 * Thomas H. Cormen, Charles E. Leiserson, Ronald L. Rivest, and
 * Clifford Stein. Introduction to Algorithms, Third Edition. The MIT
 * Press, September 2009.
 */

#define _BSD_SOURCE
#define _LGPL_SOURCE

#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include <urcu/rcurbtree.h>
#include <urcu-pointer.h>
#include <urcu-call-rcu.h>

#define DEBUG

#ifdef DEBUG
#define dbg_printf(args...)	printf(args)
#else
#define dbg_printf(args...)
#endif

/*
 * Undefine this to enable the non-RCU rotate and transplant functions
 * (for debugging).
 */
#define RBTREE_RCU_SUPPORT_ROTATE_LEFT
#define RBTREE_RCU_SUPPORT_ROTATE_RIGHT
#define RBTREE_RCU_SUPPORT_TRANSPLANT

#ifdef EXTRA_DEBUG
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t outer_mutex = PTHREAD_MUTEX_INITIALIZER;

static
void lock_outer_mutex(void)
{
	pthread_mutex_lock(&outer_mutex);
}

static
void unlock_outer_mutex(void)
{
	pthread_mutex_unlock(&outer_mutex);
}

static
void lock_test_mutex(void)
{
	pthread_mutex_lock(&test_mutex);
}

static
void unlock_test_mutex(void)
{
	pthread_mutex_unlock(&test_mutex);
}
#endif

static
void set_decay(struct rcu_rbtree_node *x, struct rcu_rbtree_node *xc)
{
	x->decay_next = xc;
}

static
struct rcu_rbtree_node *get_decay(struct rcu_rbtree_node *x)
{
	if (!x)
		return NULL;
	while (x->decay_next)
		x = x->decay_next;
	return x;
}

static
struct rcu_rbtree_node *is_decay(struct rcu_rbtree_node *x)
{
	return x->decay_next;
}

static
struct rcu_rbtree_node *dup_decay_node(struct rcu_rbtree *rbtree,
				struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xc;

	if (rcu_rbtree_is_nil(x))
		return x;

	xc = rbtree->rballoc();
	memcpy(xc, x, sizeof(struct rcu_rbtree_node));
	xc->decay_next = NULL;
	set_decay(x, xc);
	call_rcu(&x->head, rbtree->rbfree);
	return xc;
}

/*
 * TODO
 * Deal with memory allocation errors.
 * Can be ensured by reserving a pool of memory entries before doing the
 * insertion, which will have to be function of number of
 * transplantations/rotations required for the operation.
 */

static
void show_tree(struct rcu_rbtree *rbtree)
{
	struct rcu_rbtree_node *node;

	node = rcu_rbtree_min(rbtree, rbtree->root);
	while (!rcu_rbtree_is_nil(node)) {
		assert(!is_decay(node));
		printf("{ 0x%lX p:%lX r:%lX l:%lX %s %s %s} ",
			(unsigned long)node->key,
			(unsigned long) node->p->key,
			(unsigned long) node->right->key,
			(unsigned long) node->left->key,
			node->color ? "red" : "black",
			node->pos ? "right" : "left",
			node->nil ? "nil" : "");
		node = rcu_rbtree_next(rbtree, node);
	}
	printf("\n");
}

static
struct rcu_rbtree_node *make_nil(struct rcu_rbtree *rbtree)
{
	return &rbtree->nil_node;
}

/*
 * Iterative rbtree search.
 */
struct rcu_rbtree_node* rcu_rbtree_search(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *k)
{
	x = rcu_dereference(x);

	while (!rcu_rbtree_is_nil(x) && k != x->key) {
		usleep(10);
		if (rbtree->comp(k, x->key) < 0)
			x = rcu_dereference(x->left);
		else
			x = rcu_dereference(x->right);
	}
	return x;
}

static
struct rcu_rbtree_node *rcu_rbtree_min_dup_decay(struct rcu_rbtree *rbtree,
						 struct rcu_rbtree_node *x,
						 struct rcu_rbtree_node **zr)
{
	struct rcu_rbtree_node *xl;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(x)) {
		*zr = x;
		return x;
	} else
		*zr = x = dup_decay_node(rbtree, x);

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left))) {
		x = dup_decay_node(rbtree, xl);
		x->p = get_decay(x->p);
		x->p->left = get_decay(x->p->left);
	}
	return x;
}

static
struct rcu_rbtree_node *rcu_rbtree_min_update_decay(struct rcu_rbtree *rbtree,
						    struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xl;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(x))
		return x;
	else {
		x->right->p = get_decay(x->right->p);
		x->left->p = get_decay(x->left->p);
	}

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left))) {
		x = xl;
		xl->right->p = get_decay(xl->right->p);
		xl->left->p = get_decay(xl->left->p);
	}
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_min(struct rcu_rbtree *rbtree,
				       struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xl;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(x))
		return x;

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left)))
		x = xl;
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_max(struct rcu_rbtree *rbtree,
				       struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xr;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(x))
		return x;

	while (!rcu_rbtree_is_nil(xr = rcu_dereference(x->right)))
		x = xr;
	return x;
}

/*
 * FIXME:
 * Updates should wait for a grace period between update of the
 * redirect pointer and update of the parent child pointer. This is to make sure
 * that no reference to the old entry exist.
 */

/*
 * next and prev need to have mutex held to ensure that parent pointer is
 * coherent.
 */
struct rcu_rbtree_node *rcu_rbtree_next(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xr, *y;

	x = rcu_dereference(x);

	if (!rcu_rbtree_is_nil(xr = rcu_dereference(x->right)))
		return rcu_rbtree_min(rbtree, xr);
	y = rcu_dereference(x->p);
	while (!rcu_rbtree_is_nil(y) && x->pos == IS_RIGHT) {
		x = y;
		y = rcu_dereference(y->p);
	}
	return y;
}

struct rcu_rbtree_node *rcu_rbtree_prev(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xl, *y;

	x = rcu_dereference(x);

	if (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left)))
		return rcu_rbtree_min(rbtree, xl);
	y = rcu_dereference(x->p);
	while (!rcu_rbtree_is_nil(y) && x->pos == IS_LEFT) {
		x = y;
		y = rcu_dereference(y->p);
	}
	return y;
}

/*
 * We have to ensure these assumptions are correct for prev/next
 * traversal:
 *
 * with x being a right child, the assumption that:
 *   x->p->right == x
 * or if x is a left child, the assumption that:
 *   x->p->left == x
 *
 * This explains why we have to allocate a vc copy of the node for left_rotate,
 * right_rotate and transplant operations.
 *
 * We always ensure that the right/left child and correct parent is set in the
 * node copies *before* we reparent the children and make the upper-level point
 * to the copy.
 */

/* RCU: copy x and y, atomically point to new versions. GC old. */
/* Should be eventually followed by a cmm_smp_wmc() */

#ifdef RBTREE_RCU_SUPPORT_ROTATE_LEFT

static
void left_rotate(struct rcu_rbtree *rbtree,
		 struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y, *y_left, *x_p;
	unsigned int x_pos;

	y = x->right;
	y_left = y->left;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_left = dup_decay_node(rbtree, y_left);

	x_pos = x->pos;
	x_p = x->p;

	/* Internal node modifications */
	x->right = y_left;
	y->p = x->p;
	y->pos = x->pos;
	x->pos = IS_LEFT;
	y->left = x;
	x->p = y;
	if (!rcu_rbtree_is_nil(y_left)) {
		y_left->p = x;
		y_left->pos = IS_RIGHT;
	}

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(x_p))
		_CMM_STORE_SHARED(rbtree->root, y);
	else if (x_pos == IS_LEFT)
		_CMM_STORE_SHARED(x_p->left, y);
	else
		_CMM_STORE_SHARED(x_p->right, y);

	/* Point children to new copy (parent only used by updates/next/prev) */
	x->left->p = get_decay(x->left->p);
	y->right->p = get_decay(y->right->p);
	if (!rcu_rbtree_is_nil(y_left)) {
		y_left->right->p = get_decay(y_left->right->p);
		y_left->left->p = get_decay(y_left->left->p);
	}

	/* Sanity checks */
	assert(y == rbtree->root || y->p->left == y || y->p->right == y);
	assert(x == rbtree->root || x->p->left == x || x->p->right == x);
	assert(rcu_rbtree_is_nil(x->right) || x->right->p == x);
	assert(rcu_rbtree_is_nil(x->left) || x->left->p == x);
	assert(rcu_rbtree_is_nil(y->right) || y->right->p == y);
	assert(rcu_rbtree_is_nil(y->left) || y->left->p == y);
	assert(!is_decay(rbtree->root));
	assert(!is_decay(x));
	assert(!is_decay(y));
	assert(!is_decay(x->right));
	assert(!is_decay(x->left));
	assert(!is_decay(y->right));
	assert(!is_decay(y->left));
}

#else

/* non-rcu version */
static
void left_rotate(struct rcu_rbtree *rbtree,
		 struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;

	lock_test_mutex();
	y = x->right;
	x->right = y->left;
	if (!rcu_rbtree_is_nil(y->left)) {
		y->left->p = x;
		y->left->pos = IS_RIGHT;
	}
	y->p = x->p;
	if (rcu_rbtree_is_nil(x->p))
		rbtree->root = y;
	else if (x == x->p->left) {
		x->p->left = y;
		y->pos = IS_LEFT;
	} else {
		x->p->right = y;
		y->pos = IS_RIGHT;
	}
	y->left = x;
	x->pos = IS_LEFT;
	x->p = y;
	unlock_test_mutex();
}

#endif

#ifdef RBTREE_RCU_SUPPORT_ROTATE_RIGHT
static
void right_rotate(struct rcu_rbtree *rbtree,
		  struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y, *y_right, *x_p;
	unsigned int x_pos;

	y = x->left;
	y_right = y->right;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_right = dup_decay_node(rbtree, y_right);

	x_pos = x->pos;
	x_p = x->p;

	/* Internal node modifications */
	x->left = y_right;
	y->p = x->p;
	y->pos = x->pos;
	x->pos = IS_RIGHT;
	y->right = x;
	x->p = y;
	if (!rcu_rbtree_is_nil(y_right)) {
		y_right->p = x;
		y_right->pos = IS_LEFT;
	}

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(x_p))
		_CMM_STORE_SHARED(rbtree->root, y);
	else if (x_pos == IS_RIGHT)
		_CMM_STORE_SHARED(x_p->right, y);
	else
		_CMM_STORE_SHARED(x_p->left, y);

	/* Point children to new copy (parent only used by updates/next/prev) */
	x->right->p = get_decay(x->right->p);
	y->left->p = get_decay(y->left->p);
	if (!rcu_rbtree_is_nil(y_right)) {
		y_right->left->p = get_decay(y_right->left->p);
		y_right->right->p = get_decay(y_right->right->p);
	}

	/* Sanity checks */
	assert(y == rbtree->root || y->p->right == y || y->p->left == y);
	assert(x == rbtree->root || x->p->right == x || x->p->left == x);
	assert(rcu_rbtree_is_nil(x->left) || x->left->p == x);
	assert(rcu_rbtree_is_nil(x->right) || x->right->p == x);
	assert(rcu_rbtree_is_nil(y->left) || y->left->p == y);
	assert(rcu_rbtree_is_nil(y->right) || y->right->p == y);
	assert(!is_decay(rbtree->root));
	assert(!is_decay(x));
	assert(!is_decay(y));
	assert(!is_decay(x->left));
	assert(!is_decay(x->right));
	assert(!is_decay(y->left));
	assert(!is_decay(y->right));
}

#else

/* non-rcu version */
static
void right_rotate(struct rcu_rbtree *rbtree,
		  struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;

	lock_test_mutex();
	y = x->left;
	x->left = y->right;
	if (!rcu_rbtree_is_nil(y->right)) {
		y->right->p = x;
		y->right->pos = IS_LEFT;
	}
	y->p = x->p;
	if (rcu_rbtree_is_nil(x->p))
		rbtree->root = y;
	else if (x == x->p->right) {
		x->p->right = y;
		y->pos = IS_RIGHT;
	} else {
		x->p->left = y;
		y->pos = IS_LEFT;
	}
	y->right = x;
	x->pos = IS_RIGHT;
	x->p = y;
	unlock_test_mutex();
}

#endif

static void rcu_rbtree_insert_fixup(struct rcu_rbtree *rbtree,
				    struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *y;

	dbg_printf("insert fixup %p\n", z->key);
	assert(!is_decay(rbtree->root));

	while (z->p->color == COLOR_RED) {
		if (z->p == z->p->p->left) {
			y = z->p->p->right;
			if (y->color == COLOR_RED) {
				z->p->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				z = z->p->p;
			} else {
				if (z == z->p->right) {
					z = z->p;
					left_rotate(rbtree, z);
					z = get_decay(z);
					assert(!is_decay(rbtree->root));
				}
				z->p->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				assert(!is_decay(z));
				assert(!is_decay(z->p));
				assert(!is_decay(z->p->p));
				right_rotate(rbtree, z->p->p);
				assert(!is_decay(z));
				assert(!is_decay(rbtree->root));
			}
		} else {
			y = z->p->p->left;
			if (y->color == COLOR_RED) {
				z->p->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				z = z->p->p;
			} else {
				if (z == z->p->left) {
					z = z->p;
					right_rotate(rbtree, z);
					z = get_decay(z);
					assert(!is_decay(rbtree->root));
				}
				z->p->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				left_rotate(rbtree, z->p->p);
				assert(!is_decay(z));
				assert(!is_decay(rbtree->root));
			}
		}
	}
	rbtree->root->color = COLOR_BLACK;
}

/*
 * rcu_rbtree_insert - Insert a node in the RCU rbtree
 *
 * Returns 0 on success, or < 0 on error.
 */
int rcu_rbtree_insert(struct rcu_rbtree *rbtree,
		      struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *x, *y;

	dbg_printf("insert %p\n", z->key);
	assert(!is_decay(rbtree->root));

	y = make_nil(rbtree);
	if (!rbtree->root)
		rbtree->root = make_nil(rbtree);
	x = rbtree->root;
	while (!rcu_rbtree_is_nil(x)) {
		y = x;
		if (rbtree->comp(z->key, x->key) < 0)
			x = x->left;
		else
			x = x->right;
	}

	z->p = y;

	z->left = make_nil(rbtree);
	z->right = make_nil(rbtree);
	z->color = COLOR_RED;
	z->nil = 0;
	z->decay_next = NULL;

	if (rcu_rbtree_is_nil(y))
		z->pos = IS_RIGHT;	/* arbitrary for root node */
	else if (rbtree->comp(z->key, y->key) < 0)
		z->pos = IS_LEFT;
	else
		z->pos = IS_RIGHT;

	/*
	 * Order stores to z (children/parents) before stores that will make it
	 * visible to the rest of the tree.
	 */
	cmm_smp_wmb();

	if (rcu_rbtree_is_nil(y))
		_CMM_STORE_SHARED(rbtree->root, z);
	else if (rbtree->comp(z->key, y->key) < 0)
		_CMM_STORE_SHARED(y->left, z);
	else
		_CMM_STORE_SHARED(y->right, z);
	rcu_rbtree_insert_fixup(rbtree, z);
	/*
	 * Make sure to commit all _CMM_STORE_SHARED() for non-coherent caches.
	 */
	cmm_smp_wmc();
	show_tree(rbtree);

	return 0;
}

/*
 * Transplant v into u position.
 */

#ifdef RBTREE_RCU_SUPPORT_TRANSPLANT

static
void rcu_rbtree_transplant(struct rcu_rbtree *rbtree,
			struct rcu_rbtree_node *u,
			struct rcu_rbtree_node *v)
{
	dbg_printf("transplant %p\n", v->key);

	if (!rcu_rbtree_is_nil(v))
		v = dup_decay_node(rbtree, v);

	if (rcu_rbtree_is_nil(u->p)) {
		v->p = u->p;
		cmm_smp_wmb();	/* write into node before publish */
		_CMM_STORE_SHARED(rbtree->root, v);
	} else {
		v->pos = u->pos;
		v->p = u->p;
		cmm_smp_wmb();	/* write into node before publish */
		if (u->pos == IS_LEFT)
			_CMM_STORE_SHARED(u->p->left, v);
		else
			_CMM_STORE_SHARED(u->p->right, v);
	}

	/* Point children to new copy (parent only used by updates/next/prev) */
	if (!rcu_rbtree_is_nil(v)) {
		v->right->p = get_decay(v->right->p);
		v->left->p = get_decay(v->left->p);
	}
	assert(!is_decay(rbtree->root));
}

#else

/* Non-RCU version */
static
void rcu_rbtree_transplant(struct rcu_rbtree *rbtree,
			   struct rcu_rbtree_node *u,
			   struct rcu_rbtree_node *v)
{
	dbg_printf("transplant %p\n", v->key);

	lock_test_mutex();
	if (rcu_rbtree_is_nil(u->p))
		rbtree->root = v;
	else if (u == u->p->left) {
		u->p->left = v;
		v->pos = IS_LEFT;
	} else {
		u->p->right = v;
		v->pos = IS_RIGHT;
	}
	v->p = u->p;
	unlock_test_mutex();
}

#endif

static void rcu_rbtree_remove_fixup(struct rcu_rbtree *rbtree,
				    struct rcu_rbtree_node *x)
{
	dbg_printf("remove fixup %p\n", x->key);

	while (x != rbtree->root && x->color == COLOR_BLACK) {
		assert(!is_decay(x->p));
		assert(!is_decay(x->p->left));
		if (x == x->p->left) {
			struct rcu_rbtree_node *w;

			w = x->p->right;

			if (w->color == COLOR_RED) {
				w->color = COLOR_BLACK;
				x->p->color = COLOR_RED;
				left_rotate(rbtree, x->p);
				x = get_decay(x);
				assert(!is_decay(rbtree->root));
				w = x->p->right;
			}
			if (w->left->color == COLOR_BLACK
			    && w->right->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = x->p;
				assert(!is_decay(rbtree->root));
				assert(!is_decay(x));
			} else {
				if (w->right->color == COLOR_BLACK) {
					w->left->color = COLOR_BLACK;
					w->color = COLOR_RED;
					right_rotate(rbtree, w);
					assert(!is_decay(rbtree->root));
					x = get_decay(x);
					w = x->p->right;
				}
				w->color = x->p->color;
				x->p->color = COLOR_BLACK;
				w->right->color = COLOR_BLACK;
				left_rotate(rbtree, x->p);
				assert(!is_decay(rbtree->root));
				x = rbtree->root;
			}
		} else {
			struct rcu_rbtree_node *w;

			w = x->p->left;

			if (w->color == COLOR_RED) {
				w->color = COLOR_BLACK;
				x->p->color = COLOR_RED;
				right_rotate(rbtree, x->p);
				assert(!is_decay(rbtree->root));
				x = get_decay(x);
				w = x->p->left;
			}
			if (w->right->color == COLOR_BLACK
			    && w->left->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = x->p;
				assert(!is_decay(rbtree->root));
				assert(!is_decay(x));
			} else {
				if (w->left->color == COLOR_BLACK) {
					w->right->color = COLOR_BLACK;
					w->color = COLOR_RED;
					left_rotate(rbtree, w);
					assert(!is_decay(rbtree->root));
					x = get_decay(x);
					w = x->p->left;
				}
				w->color = x->p->color;
				x->p->color = COLOR_BLACK;
				w->left->color = COLOR_BLACK;
				right_rotate(rbtree, x->p);
				assert(!is_decay(rbtree->root));
				x = rbtree->root;
			}
		}
	}
	x->color = COLOR_BLACK;
}

/*
 * Delete z. All non-copied children left/right positions are unchanged.
 */
static
void rcu_rbtree_remove_nonil(struct rcu_rbtree *rbtree,
			     struct rcu_rbtree_node *z,
			     struct rcu_rbtree_node *y)
{
	struct rcu_rbtree_node *x;

	dbg_printf("remove nonil %p\n", z->key);
	show_tree(rbtree);

	assert(!is_decay(z));
	assert(!is_decay(y));
	assert(!is_decay(y->right));
	assert(!is_decay(y->p));
	x = y->right;
	assert(!is_decay(x));
	if (y->p == z) {
		y = dup_decay_node(rbtree, y);
		x->p = y;
		y->left = z->left;
		rcu_rbtree_transplant(rbtree, z, y);
	} else {
		struct rcu_rbtree_node *oy_right, *z_right;

		/*
		 * Need to make sure y is always visible by readers.
		 */
		y = rcu_rbtree_min_dup_decay(rbtree, z->right, &z_right);
		assert(!is_decay(y));
		assert(!is_decay(z));
		oy_right = y->right;
		y->right = z_right;
		y->right->p = y;
		assert(!is_decay(z->left));
		y->left = z->left;
		assert(!is_decay(oy_right));
		rcu_rbtree_transplant(rbtree, y, oy_right);
		rcu_rbtree_transplant(rbtree, z, y);
		/* Update children */
		(void) rcu_rbtree_min_update_decay(rbtree, y->right);
	}
	y = get_decay(y);
	assert(!is_decay(z));
	assert(!is_decay(z->left));
	y->color = z->color;
	y->left->p = y;
	y->right->p = get_decay(y->right->p);
	assert(!is_decay(y->left));
	assert(!is_decay(y->right));
}

int rcu_rbtree_remove(struct rcu_rbtree *rbtree,
		      struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *x, *y;
	unsigned int y_original_color;

	assert(!is_decay(rbtree->root));
	dbg_printf("remove %p\n", z->key);
	show_tree(rbtree);

	assert(!is_decay(z));
	y = z;
	y_original_color = y->color;

	if (rcu_rbtree_is_nil(z->left)) {
		rcu_rbtree_transplant(rbtree, z, z->right);
		assert(!is_decay(z));
		x = get_decay(z->right);
		show_tree(rbtree);
	} else if (rcu_rbtree_is_nil(z->right)) {
		rcu_rbtree_transplant(rbtree, z, z->left);
		assert(!is_decay(z));
		x = get_decay(z->left);
		show_tree(rbtree);
	} else {
		y = rcu_rbtree_min(rbtree, z->right);
		assert(!is_decay(y));
		y_original_color = y->color;
		x = y->right;
		rcu_rbtree_remove_nonil(rbtree, z, y);
		x = get_decay(x);
		show_tree(rbtree);
	}
	if (y_original_color == COLOR_BLACK)
		rcu_rbtree_remove_fixup(rbtree, x);
	show_tree(rbtree);
	/*
	 * Commit all _CMM_STORE_SHARED().
	 */
	cmm_smp_wmc();

	return 0;
}
