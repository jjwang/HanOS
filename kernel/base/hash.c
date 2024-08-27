#include <stdbool.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <base/hash.h>
#include <base/kmalloc.h>
#include <sys/panic.h>

static int64_t ht_hashcode(ht_t *ht, int64_t key)
{
    return key % ht->size;
}

void ht_init_core(ht_t *ht, uint64_t size)
{
    ht->size = size;
    ht->array = (ht_item_t*)kmalloc(ht->size * sizeof(ht_item_t));
    for (size_t i = 0; i < ht->size; i++) {
        ht->array[i].key = -1;
        ht->array[i].data = NULL;
    }
}

void ht_init(ht_t *ht)
{
    ht_init_core(ht, HT_DEFAULT_ARRAY_SIZE);
}

void *ht_search(ht_t *ht, int64_t key)
{
    uint64_t loop_count = 0;

    /* get the hash */
    int64_t index = ht_hashcode(ht, key);
    
    /* move in array until an empty */
    while(ht->array[index].key != -1 && ht->array[index].data != NULL) {
        if(ht->array[index].key == key)
            return ht->array[index].data; 
   
        /* go to next cell */
        ++index;
        
        /* wrap around the table */
        index %= ht->size;

        /* At most loop the table twice */
        if (++loop_count >= ht->size * 2) break;
    }
    
    return NULL;
}

bool ht_insert_core(ht_t *ht, int64_t key, void *data, bool allow_realloc)
{
    uint64_t loop_count = 0;

    /* get the hash */
    int64_t index = ht_hashcode(ht, key);

    /* move in array until an empty or deleted cell */
    while(ht->array[index].key != -1 && ht->array[index].data != NULL) {
        /* go to next cell */
        ++index;
        
        /* wrap around the table */
        index %= ht->size;

        /* At most loop the table twice */
        if (++loop_count >= ht->size * 2) {
            /* TODO: we should increase the hash table size here */
            kpanic("hash: current size %d is not enough\n", ht->size);
            return false;
        }
    }
    
    ht->array[index].key = key;
    ht->array[index].data = data;

    return true;
}

bool ht_insert(ht_t *ht, int64_t key, void *data)
{
    return ht_insert_core(ht, key, data, true);
}

void *ht_delete(ht_t *ht, int64_t key)
{
    uint64_t loop_count = 0;

    /* get the hash */ 
    int64_t index = ht_hashcode(ht, key);

    /* move in array until an empty */
    while(ht->array[index].key != -1 && ht->array[index].data != NULL) {
        if(ht->array[index].key == key) {
            void *temp = ht->array[index].data; 
            /* assign a dummy item at deleted position */
            ht->array[index].key = -1; 
            ht->array[index].data = NULL;
            return temp;
        }
        
        /* go to next cell */
        ++index;
        
        /* wrap around the table */
        index %= ht->size;

        /* At most loop the table twice */
        if (++loop_count >= ht->size * 2) return NULL;
    } 
    
    return NULL;
}

