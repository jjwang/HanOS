#pragma once

#define HT_DEFAULT_ARRAY_SIZE       128

typedef struct {
   int64_t  key;
   void     *data;
} ht_item_t;

typedef struct {
    uint64_t size;
    ht_item_t *array;
} ht_t;

void ht_init(ht_t *ht, uint64_t size);
void *ht_search(ht_t *ht, int64_t key);
bool ht_insert(ht_t *ht, int64_t key, void *data);
void *ht_delete(ht_t *ht, int64_t key);

