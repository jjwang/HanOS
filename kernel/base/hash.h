#pragma once

#define HT_ARRAY_SIZE           20

typedef struct {
   int64_t  key;
   void     *data;
} ht_item_t;

typedef struct {
    ht_item_t array[HT_ARRAY_SIZE];
} ht_t;

void ht_init(ht_t *ht);
void *ht_search(ht_t *ht, int64_t key);
bool ht_insert(ht_t *ht, int64_t key, void *data);
void *ht_delete(ht_t *ht, int64_t key);

