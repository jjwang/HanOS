#include <stdbool.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <base/hash.h>

static int64_t ht_hashcode(int64_t key)
{
    return key % HT_ARRAY_SIZE;
}

void ht_init(ht_t *ht)
{
    for (size_t i = 0; i < HT_ARRAY_SIZE; i++) {
        ht->array[i].key = -1;
        ht->array[i].data = NULL;
    }
}

void *ht_search(ht_t *ht, int64_t key)
{
    int64_t loop_count = 0;

    /* get the hash */
    int64_t index = ht_hashcode(key);
    
    /* move in array until an empty */
    while(ht->array[index].key != -1 && ht->array[index].data != NULL) {
        if(ht->array[index].key == key)
            return ht->array[index].data; 
   
        /* go to next cell */
        ++index;
        
        /* wrap around the table */
        index %= HT_ARRAY_SIZE;

        /* At most loop the table twice */
        if (++loop_count >= HT_ARRAY_SIZE * 2) break;
    }
    
    return NULL;
}

bool ht_insert(ht_t *ht, int64_t key, void *data)
{
    int64_t loop_count = 0;

    /* get the hash */
    int64_t index = ht_hashcode(key);

    /* move in array until an empty or deleted cell */
    while(ht->array[index].key != -1 && ht->array[index].data != NULL) {
        /* go to next cell */
        ++index;
        
        /* wrap around the table */
        index %= HT_ARRAY_SIZE;

        /* At most loop the table twice */
        if (++loop_count >= HT_ARRAY_SIZE * 2) {
            /* TODO: we should increase the hash table size here */
            return false;
        }
    }
    
    ht->array[index].key = key;
    ht->array[index].data = data;

    return true;
}

void *ht_delete(ht_t *ht, int64_t key)
{
    int64_t loop_count = 0;

    /* get the hash */ 
    int64_t index = ht_hashcode(key);

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
        index %= HT_ARRAY_SIZE;

        /* At most loop the table twice */
        if (++loop_count >= HT_ARRAY_SIZE * 2) return NULL;
    } 
    
    return NULL;
}

