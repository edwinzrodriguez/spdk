/*
 *   Copyright (c) 2021 NetApp, Inc. All rights reserved.
 */

/* TODO still need to determine best way to manage values in/out without copy.  What is data lifecycle? */
/* these return values defined by errno */
int kv_malloc_get(uint8_t *key, uint32_t key_size, void **value_out, uint32_t *value_size);
int kv_malloc_insert(uint8_t *key, uint32_t key_size, void *value_in, uint32_t value_size);
int kv_malloc_delete(uint8_t *key, uint32_t key_size);
int kv_malloc_list(void /* TODO args */);

int kv_malloc_store_create(void /* TODO size params */);
void kv_malloc_store_destroy(void);
