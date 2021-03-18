/*
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 */


/*
 * Use an AVL tree as the backend for KV Malloc
 */

#include "spdk/bdev_module.h"
#include "bdev_kv_malloc_store.h"

int
kv_malloc_store_create(void)
{
	/* TODO */
	return 0;
}

void
kv_malloc_store_destroy(void)
{
	/* TODO */
}

/* TODO still need to determine best way to manage values in/out without copy.  What is data lifecycle? */
int
kv_malloc_get(uint8_t *key, uint32_t key_size, void **value_out, uint32_t *value_size)
{
	/* TODO */
	return 0;
}

int
kv_malloc_insert(uint8_t *key, uint32_t key_size, void *value_in, uint32_t value_size)
{
	/* TODO */
	return 0;
}

int
kv_malloc_delete(uint8_t *key, uint32_t key_size)
{
	/* TODO */
	return 0;
}

int
kv_malloc_list(void /* TODO args */)
{
	/* TODO */
	return 0;
}
