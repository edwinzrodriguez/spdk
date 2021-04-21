/*
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 */

/* TODO still need to determine best way to manage values in/out without copy.  What is data lifecycle? */
/* these return values defined by errno */

/*
 * kv_malloc_get
 * pass NULL for value_out to simply test for existence of a key without actually
 * fetching the value
 */
int kv_malloc_get(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size, void **value_out,
		  uint32_t *value_size);
int kv_malloc_insert(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size, void *value_in,
		     uint32_t value_size);
int kv_malloc_delete(struct kv_malloc_bdev *bdev, uint8_t *key, uint32_t key_size);
int kv_malloc_list(struct kv_malloc_bdev *bdev /* TODO args */);

int kv_malloc_store_create(struct kv_malloc_bdev *bdev /* TODO size params */);
void kv_malloc_store_destroy(struct kv_malloc_bdev *bdev);
