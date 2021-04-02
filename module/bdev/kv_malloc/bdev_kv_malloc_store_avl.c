/*
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 */


/*
 * Use an AVL tree as the backend for KV Malloc
 */

#include "spdk/bdev_module.h"
#include "bdev_kv_malloc_internal.h"
#include "bdev_kv_malloc_store.h"

struct store_avl_metadata {
	/* TODO main data store */
	/* TODO relevant freelists */
	/* TODO rwlock to cover list and freelists */
};

static struct store_avl_metadata *
	get_metadata_struct(struct kv_malloc_bdev *bdev)
{
	return (struct store_avl_metadata *)(bdev->store_metadata);
}

int
kv_malloc_store_create(struct kv_malloc_bdev *bdev)
{
	struct store_avl_metadata *md = (struct store_avl_metadata *)malloc(sizeof(
						struct store_avl_metadata));
	bdev->store_metadata = md;
	if (!md) {
		return ENOMEM;
	}

	/* TODO init store_avl_metadata */
	return 0;
}

void
kv_malloc_store_destroy(struct kv_malloc_bdev *bdev)
{
	struct store_avl_metadata *md = get_metadata_struct(bdev);

	if (md) {
		/* TODO */
		free(md);
		bdev->store_metadata = NULL;
	}
}

/* TODO still need to determine best way to manage values in/out without copy.  What is data lifecycle? */
int
kv_malloc_get(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size, void **value_out,
	      uint32_t *value_size)
{
	struct store_avl_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	/* TODO */
	return 0;
}

int
kv_malloc_insert(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size, void *value_in,
		 uint32_t value_size)
{
	struct store_avl_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	/* TODO */
	return 0;
}

int
kv_malloc_delete(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size)
{
	struct store_avl_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	/* TODO */
	return 0;
}

int
kv_malloc_list(struct kv_malloc_bdev *bdev /* TODO args */)
{
	struct store_avl_metadata *md = get_metadata_struct(bdev);
	if (!md) {
		return ENODEV;
	}

	/* TODO */
	return 0;
}
