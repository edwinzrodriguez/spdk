/*
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 */

/*
 * bdev_kv_malloc_store_list.c
 *
 * Use a simple but slow ordered list as the backend for KV Malloc
 * to validate the API and functionality.
 *
 * Compile a different bdev_kv_malloc_store_*.c file for higher
 * performance.
 */

#include "spdk/bdev_module.h"
#include "spdk/nvme_kv.h"
#include "bdev_kv_malloc_internal.h"
#include "bdev_kv_malloc_store.h"
#include <sys/queue.h>

#ifndef KV_MALLOC_NO_LOCKS
#define DEF_LOCK(x) pthread_rwlock_t x
#define INIT_LOCK(x) pthread_rwlock_init((x), NULL)
#define WLOCK(x) pthread_rwlock_wrlock(x)
#define RLOCK(x) pthread_rwlock_rdlock(x)
#define UNLOCK(x) pthread_rwlock_unlock(x)
#define DESTROY_LOCK(x) pthread_rwlock_destroy(x)
#else
#define DEF_LOCK(x)
#define INIT_LOCK(x)
#define WLOCK(x)
#define RLOCK(x)
#define UNLOCK(x)
#define DESTROY_LOCK(x)
#endif /* KV_MALLOC_NO_LOCKS */

#define EXIT_WITH(x) retval = (x); goto done

struct list_node {
	uint32_t key_len;
	uint8_t key[KV_MAX_KEY_SIZE];
	void *val;
	uint32_t val_len;

	LIST_ENTRY(list_node) link;
};

/*
 * store_list_metadata is a per-bdev structure that is used to keep
 * track of persistent information.  In this case, it holds the list
 * containing the KV pairs, and the list lock.
 */
struct store_list_metadata {
	LIST_HEAD(list_head_type, list_node) list_head;

	/*
	 * We could use freelists to speed things up and avoid memory allocations
	 * in the typical case.  But this list implementation exists for
	 * validating the API, and not for performance.  Other data structures
	 * will be used for performance as an alternative to this one,
	 * so adding freelists here is a low priority
	 */

	DEF_LOCK(lists_lock);
};

static struct store_list_metadata *
	get_metadata_struct(struct kv_malloc_bdev *bdev)
{
	return (struct store_list_metadata *)(bdev->store_metadata);
}

/* return -1 if key < node
 * return 0 if key == node
 * return 1 if key > node
 */
static int
compare_key_to_node(uint8_t *key, uint32_t key_size, struct list_node *node)
{
	if (!node) {
		return -1;
	}
	if (key_size < node->key_len) {
		return -1;
	}
	if (key_size > node->key_len) {
		return 1;
	}

	/* key sizes are equal */
	for (uint32_t i = 0; i < key_size; i++) {
		if (key[i] < node->key[i]) {
			return -1;
		}
		if (key[i] > node->key[i]) {
			return 1;
		}
	}
	return 0;
}

static int
init_node(struct list_node *node, uint8_t *key, uint32_t key_size, void *value_in,
	  uint32_t value_size)
{
	void *new_val = NULL;

	if (!node) {
		return EINVAL;
	}
	if (value_size) {
		new_val = malloc(value_size);
		if (!new_val) {
			return ENOMEM;
		}
	}
	node->key_len = key_size;
	memcpy(node->key, key, key_size);
	node->val_len = value_size;
	node->val = new_val;
	if (new_val) {
		memcpy(node->val, value_in, value_size);
	}

	return 0;
}

static struct list_node *
make_new_node(uint8_t *key, uint32_t key_size, void *value_in, uint32_t value_size)
{
	/*
	 * Normally, we'd want to avoid allocating memory in the data path.
	 * We could use freelist(s) to speed things up in the typical case.
	 * But this list implementation exists for validating the API, and
	 * not for performance.  Other data structures will be used for
	 * performance as an alternative to this one, so adding freelists
	 * is a low priority.
	 */
	struct list_node *new_node = (struct list_node *)malloc(sizeof(struct list_node));

	if (new_node) {
		int err = init_node(new_node, key, key_size, value_in, value_size);
		if (err) {
			free(new_node);
			new_node = NULL;
		}
	}

	return new_node;
}

static void
delete_node(struct list_node *node)
{
	if (node) {
		if (node->val) {
			free(node->val);
		}
		free(node);
	}
}

int
kv_malloc_store_create(struct kv_malloc_bdev *bdev)
{
	struct store_list_metadata *md = (struct store_list_metadata *)malloc(sizeof(
			struct store_list_metadata));
	bdev->store_metadata = md;
	if (!md) {
		return ENOMEM;
	}
	INIT_LOCK(&(md->lists_lock));
	LIST_INIT(&(md->list_head));

	return 0;
}

void
kv_malloc_store_destroy(struct kv_malloc_bdev *bdev)
{
	struct list_node *np;
	struct store_list_metadata *md = get_metadata_struct(bdev);

	if (md) {
		WLOCK(&(md->lists_lock));
		while ((np = LIST_FIRST(&(md->list_head))) != NULL) {
			LIST_REMOVE(np, link);
			delete_node(np);
		}
		UNLOCK(&(md->lists_lock));
		DESTROY_LOCK(&(md->lists_lock));
		free(md);
		bdev->store_metadata = NULL;
	}
}

/* TODO unit tests
 * get from empty list
 * get beginning, middle, end
 * get with key not found
 */
int
kv_malloc_get(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size, void *buf_for_value,
	      uint32_t buffer_size, uint32_t *value_size)
{
	int retval = 0;
	bool test_existence_only = false;
	if (buf_for_value == NULL) {
		test_existence_only = true;
	}
	if (value_size != NULL) {
		*value_size = 0;
	} else if (! test_existence_only) {
		/* Need to be able to return value_size if returning a value */
		return EINVAL;
	}
	struct store_list_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	RLOCK(&(md->lists_lock));
	if (LIST_EMPTY(&(md->list_head))) {
		EXIT_WITH(ENOENT);
	}

	/* traverse list to find entry */
	for (struct list_node *np = LIST_FIRST(&(md->list_head)); np != NULL; np = LIST_NEXT(np, link)) {
		int compare = compare_key_to_node(key, key_size, np);
		if (compare == 0) {
			/* key found */
			if (!test_existence_only) {
				memcpy(buf_for_value, np->val, spdk_min(np->val_len, buffer_size));
				*value_size = np->val_len;
			}
			EXIT_WITH(0);
		} else if (compare > 0) {
			/* already passed the place where this key should have been */
			break;
		}
		/* else keep looking */
	}

	/* Not found */
	EXIT_WITH(ENOENT);

done:
	UNLOCK(&(md->lists_lock));
	return retval;
}

/* TODO unit tests
 * insert empty
 * insert beginning, middle, end
 * insert duplicate key
 */
int
kv_malloc_insert(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size, void *value_in,
		 uint32_t value_size)
{
	int retval = 0;
	struct list_node *new_node = NULL;
	struct store_list_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	/*
	 * Don't want to allocate memory while holding a lock, so assume that the common
	 * case is that the key being inserted does not already exist
	 */
	new_node = make_new_node(key, key_size, value_in, value_size);
	if (!new_node) {
		return ENOMEM;
	}

	WLOCK(&(md->lists_lock));
	if (LIST_EMPTY(&(md->list_head))) {
		LIST_INSERT_HEAD(&(md->list_head), new_node, link);
		EXIT_WITH(0);
	}

	/* traverse list and insert in order */
	struct list_node *prev = NULL;
	for (struct list_node *np = LIST_FIRST(&(md->list_head)); np != NULL; np = LIST_NEXT(np, link)) {
		int compare = compare_key_to_node(key, key_size, np);
		if (compare < 0) {
			LIST_INSERT_BEFORE(np, new_node, link);
			EXIT_WITH(0);
		} else if (compare == 0) {
			/* key already in use */
			EXIT_WITH(EEXIST);
		}
		/* else keep going.  Haven't found the position yet. */
		prev = np;
	}

	LIST_INSERT_AFTER(prev, new_node, link);
	EXIT_WITH(0);

done:
	UNLOCK(&(md->lists_lock));

	if (retval != 0) {
		delete_node(new_node);
	}
	return retval;
}

/* TODO unit tests
 * delete from empty list
 * delete beginning, middle, end
 * delete with key not found
 */
int
kv_malloc_delete(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size)
{
	int retval = 0;
	struct store_list_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	WLOCK(&(md->lists_lock));
	if (LIST_EMPTY(&(md->list_head))) {
		EXIT_WITH(ENOENT);
	}

	/* traverse list to find entry */
	for (struct list_node *np = LIST_FIRST(&(md->list_head)); np != NULL; np = LIST_NEXT(np, link)) {
		int compare = compare_key_to_node(key, key_size, np);
		if (compare == 0) {
			/* key found */
			LIST_REMOVE(np, link);
			delete_node(np);
			EXIT_WITH(0);
		} else if (compare < 0) {
			/* already passed the place where this key should have been */
			break;
		}
		/* else keep looking */
	}

	/* Not found */
	EXIT_WITH(ENOENT);

done:
	UNLOCK(&(md->lists_lock));
	return retval;
}

int
kv_malloc_list(struct kv_malloc_bdev *bdev /* TODO args */)
{
	struct store_list_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	/* TODO */
	return 0;
}
