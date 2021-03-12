/*
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 */

/*
 * Use a simple but slow list as the backend for KV Malloc
 */

#include "spdk/bdev_module.h"
#include "bdev_kv_malloc_store.h"

/* TODO still need to determine best way to manage values in/out without copy.  What is data lifecycle? */
enum spdk_bdev_io_status
kv_malloc_get(uint32_t *key, uint8_t key_size, void **value_out, uint32_t *value_size) {
	/* TODO */
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

enum spdk_bdev_io_status
kv_malloc_insert(uint32_t *key, uint8_t key_size, void *value_in, uint32_t value_size) {
	/* TODO */
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

enum spdk_bdev_io_status
kv_malloc_delete(uint32_t *key, uint8_t key_size) {
	/* TODO */
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

enum spdk_bdev_io_status
kv_malloc_list(void /* TODO args */) {
	/* TODO */
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}
