/*
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 */

/*
 * Use a simple but slow list as the backend for KV Malloc
 * to validate the API and functionality.
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

void
kv_malloc_store_init(void)
{
	LIST_INIT(&list_head);
}

void
kv_malloc_store_destroy(void)
{
	/* TODO */
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

enum spdk_bdev_io_status
kv_malloc_get(uint8_t *key, uint32_t key_size, void **value_out, uint32_t *value_size) {

	*value_out = NULL;
	*value_size = 0;

	if (LIST_EMPTY(&list_head))
	{
		return SPDK_BDEV_IO_STATUS_NOMEM; /* TODO or should this be SPDK_BDEV_IO_STATUS_FAILED? */
	}

	/* traverse list to find entry */
	for (struct list_node *np = LIST_FIRST(&list_head); np != NULL; np = LIST_NEXT(np, link))
	{
		int compare = compare_key_to_node(key, key_size, np);
		if (compare == 0) {
			/* key found */
			/* TODO want to avoid copy, but need to trust that caller won't modify it */
			*value_out = np->val;
			*value_size = np->val_len;
			return SPDK_BDEV_IO_STATUS_SUCCESS;
		} else if (compare > 0) {
			/* already passed the place where this key should have been */
			break;
		}
		/* else keep looking */
	}

	/* Not found */
	return SPDK_BDEV_IO_STATUS_NOMEM; /* TODO should be same as error above */
}

enum spdk_bdev_io_status
kv_malloc_insert(uint8_t *key, uint32_t key_size, void *value_in, uint32_t value_size) {

	/* TODO Avoid allocating memory in the data path.  Have a pre-created freelist. */
	if (LIST_EMPTY(&list_head))
	{
		struct list_node *new_node = make_new_node(key, key_size, value_in, value_size);
		if (!new_node) {
			return SPDK_BDEV_IO_STATUS_NOMEM;
		}
		LIST_INSERT_HEAD(&list_head, new_node, link);
		return SPDK_BDEV_IO_STATUS_SUCCESS;
	}

	/* traverse list and insert in order */
	struct list_node *prev = NULL;
	for (struct list_node *np = LIST_FIRST(&list_head); np != NULL; np = LIST_NEXT(np, link))
	{
		int compare = compare_key_to_node(key, key_size, np);
		if (compare < 0) {
			struct list_node *new_node = make_new_node(key, key_size, value_in, value_size);
			if (!new_node) {
				return SPDK_BDEV_IO_STATUS_NOMEM;
			}
			LIST_INSERT_BEFORE(np, new_node, link);
			return SPDK_BDEV_IO_STATUS_SUCCESS;
		} else if (compare == 0) {
			/* key already in use */
			return SPDK_BDEV_IO_STATUS_FAILED;
		}
		prev = np;
	}

	struct list_node *new_node = make_new_node(key, key_size, value_in, value_size);
	if (!new_node)
	{
		return SPDK_BDEV_IO_STATUS_NOMEM;
	}
	LIST_INSERT_AFTER(prev, new_node, link);
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

enum spdk_bdev_io_status
kv_malloc_delete(uint8_t *key, uint32_t key_size) {

	if (LIST_EMPTY(&list_head))
	{
		return SPDK_BDEV_IO_STATUS_NOMEM; /* TODO Does NVMe KV have idempotent semantics?
		                                   * Should we return success or failure if the item is already gone?
		                                   */
	}

	/* traverse list to find entry */
	for (struct list_node *np = LIST_FIRST(&list_head); np != NULL; np = LIST_NEXT(np, link))
	{
		int compare = compare_key_to_node(key, key_size, np);
		if (compare == 0) {
			/* key found */
			LIST_REMOVE(np, link);
			return SPDK_BDEV_IO_STATUS_SUCCESS;
		} else if (compare > 0) {
			/* already passed the place where this key should have been */
			break;
		}
		/* else keep looking */
	}

	/* Not found */
	return SPDK_BDEV_IO_STATUS_NOMEM; /* TODO should be same as error above */
}

enum spdk_bdev_io_status
kv_malloc_list(void /* TODO args */) {
	/* TODO */
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}
