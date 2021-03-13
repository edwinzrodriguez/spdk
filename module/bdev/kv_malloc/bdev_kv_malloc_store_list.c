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
#include "bdev_kv_malloc_store.h"
#include <sys/queue.h>

struct list_node {
	uint32_t key_len;
	uint8_t key[KV_MAX_KEY_SIZE];
	void *val;
	uint32_t val_len;

	LIST_ENTRY(list_node) link;
};

static LIST_HEAD(list_head_type, list_node) list_head;


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

static void
init_node(struct list_node *node, uint8_t *key, uint32_t key_size, void *value_in,
	  uint32_t value_size)
{
	if (node) {
		node->key_len = key_size;
		memcpy(node->key, key, key_size);
		node->val_len = value_size;
		/* TODO can we keep the current value_in memory without doing a copy? */
		node->val = &(node[1]);
		memcpy(node->val, value_in, value_size);
	}
}

static struct list_node *
make_new_node(uint8_t *key, uint32_t key_size, void *value_in, uint32_t value_size)
{
	/* TODO want to avoid allocating memory in the data path.  For now, we are also allocating
	 * space for the value, too; but we'd rather avoid copying it.
	 */
	struct list_node *new_node = (struct list_node *)malloc(sizeof(struct list_node) + value_size);

	if (new_node) {
		init_node(new_node, key, key_size, value_in, value_size);
	}

	return new_node;
}

static void
delete_node(struct list_node *node)
{
	if (node) {
		/* TODO right now the value is in the node, and they are dynamically allocated */
		free(node);
	}
}

int
kv_malloc_store_create(void)
{
	LIST_INIT(&list_head);

	return 0;
}

void
kv_malloc_store_destroy(void)
{
	struct list_node *np;
	while ((np = LIST_FIRST(&list_head)) != NULL) {
		LIST_REMOVE(np, link);
		delete_node(np);
	}
}

/* TODO unit tests
 * get from empty list
 * get beginning, middle, end
 * get with key not found
 */
int
kv_malloc_get(uint8_t *key, uint32_t key_size, void **value_out, uint32_t *value_size)
{

	*value_out = NULL;
	*value_size = 0;

	if (LIST_EMPTY(&list_head)) {
		return ENOENT;
	}

	/* traverse list to find entry */
	for (struct list_node *np = LIST_FIRST(&list_head); np != NULL; np = LIST_NEXT(np, link)) {
		int compare = compare_key_to_node(key, key_size, np);
		if (compare == 0) {
			/* key found */
			/* TODO want to avoid copy, but need to trust that caller won't modify it */
			*value_out = np->val;
			*value_size = np->val_len;
			return 0;
		} else if (compare > 0) {
			/* already passed the place where this key should have been */
			break;
		}
		/* else keep looking */
	}

	/* Not found */
	return ENOENT;
}

/* TODO unit tests
 * insert empty
 * insert beginning, middle, end
 * insert duplicate key
 */
int
kv_malloc_insert(uint8_t *key, uint32_t key_size, void *value_in, uint32_t value_size)
{

	/* TODO Avoid allocating memory in the data path.  Have a pre-created freelist. */
	if (LIST_EMPTY(&list_head)) {
		struct list_node *new_node = make_new_node(key, key_size, value_in, value_size);
		if (!new_node) {
			return ENOMEM;
		}
		LIST_INSERT_HEAD(&list_head, new_node, link);
		return 0;
	}

	/* traverse list and insert in order */
	struct list_node *prev = NULL;
	for (struct list_node *np = LIST_FIRST(&list_head); np != NULL; np = LIST_NEXT(np, link)) {
		int compare = compare_key_to_node(key, key_size, np);
		if (compare < 0) {
			struct list_node *new_node = make_new_node(key, key_size, value_in, value_size);
			if (!new_node) {
				return ENOMEM;
			}
			LIST_INSERT_BEFORE(np, new_node, link);
			return 0;
		} else if (compare == 0) {
			/* key already in use */
			return EEXIST;
		}
		/* else keep going.  Haven't found the position yet. */
		prev = np;
	}

	struct list_node *new_node = make_new_node(key, key_size, value_in, value_size);
	if (!new_node) {
		return ENOMEM;
	}
	LIST_INSERT_AFTER(prev, new_node, link);
	return 0;
}

/* TODO unit tests
 * delete from empty list
 * delete beginning, middle, end
 * delete with key not found
 */
enum spdk_bdev_io_status
kv_malloc_delete(uint8_t *key, uint32_t key_size) {

	if (LIST_EMPTY(&list_head))
	{
		return ENOENT;
	}

	/* traverse list to find entry */
	for (struct list_node *np = LIST_FIRST(&list_head); np != NULL; np = LIST_NEXT(np, link))
	{
		int compare = compare_key_to_node(key, key_size, np);
		if (compare == 0) {
			/* key found */
			LIST_REMOVE(np, link);
			delete_node(np);
			return 0;
		} else if (compare < 0) {
			/* already passed the place where this key should have been */
			break;
		}
		/* else keep looking */
	}

	/* Not found */
	return ENOENT;
}

int
kv_malloc_list(void /* TODO args */)
{
	/* TODO */
	return 0;
}
