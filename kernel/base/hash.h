/**-----------------------------------------------------------------------------

 @file    hash.h
 @brief   Hash table definitions
 @details
 @verbatim

  This file contains the type definitions and function declarations for the
  hash table implementation used within the HanOS kernel. It includes the
  structure definitions for hash table items and the hash table itself,
  as well as the function prototypes for initializing, searching, inserting,
  and deleting entries in the hash table.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
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

