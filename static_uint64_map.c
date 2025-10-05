#include "static_uint64_map.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#define debugf(fmt, ...) \
  do {                   \
  } while (0)
#else
#define debugf printf
#endif

typedef struct key_value {
  uint64_t key;
  uint64_t value;
} key_value_t;

// Layout of hash table:
// Aligned by one cache line (64 bytes), 4 elements of key_value_t.
static const size_t items_in_bucket = 4;
static const size_t alignment = 64;

typedef struct hm_u64map_database {
  // Hash table. See above for the layout.
  key_value_t* hash_table;

  // Factors for multiplication in hm_u64map_hash64.
  uint64_t factor1, factor2;

  // Mask to go from hash64 to hash_table index.
  uint64_t mask_for_hash;
} hm_u64map_database_t;

// https://stackoverflow.com/a/6867612
static inline uint64_t hm_u64map_hash64(const hm_u64map_database_t* db,
                                        uint64_t key) {
  key ^= key >> 33;
  key *= db->factor1;
  key ^= key >> 33;
  key *= db->factor2;
  key ^= key >> 33;
  return key;
}

static inline int round_up_to_power_of_2(int n) {
  int power = 1;
  while (power < n) {
    power *= 2;
  }
  return power;
}

static inline char* align64(char* addr) {
  return (char*)(((uintptr_t)(addr) & ~(alignment - 1)) + alignment);
}

static inline int hash_table_buckets(unsigned int elements) {
  // Calibrate the number of buckets to make probability of having a 5-way
  // collision less than 1 (e.g. 99.5%), so the number of attempts in
  // compilation is not too high. The probability of having a 5-way collision
  // among N elements put into M 4-bucket groups is 1 - exp(-(N choose 5) /
  // M^4). For 10k elements the factor of 2 works well.

  int result = round_up_to_power_of_2(elements) * items_in_bucket * 2;
  if (result < 16) {
    result = 16;
  }

  return result;
}

static inline uint64_t get_buckets(const hm_u64map_database_t* db) {
  return db->mask_for_hash + 1 + 3;
}

static inline void clear_hash_table(hm_u64map_database_t* db) {
  uint64_t buckets = get_buckets(db);
  for (int i = 0; i < buckets; i++) {
    db->hash_table[i].key = 0;
    db->hash_table[i].value = 0;
  }
}

static inline size_t get_db_place(int buckets) {
  return sizeof(hm_u64map_database_t) + buckets * sizeof(key_value_t) +
         alignment;
}

HM_PUBLIC_API
size_t HM_CDECL hm_u64map_db_place_size(unsigned int elements) {
  return get_db_place(hash_table_buckets(elements));
}

static inline int comp_key_value(const void* elem1, const void* elem2) {
  key_value_t* f = (key_value_t*)elem1;
  key_value_t* s = (key_value_t*)elem2;
  return (f->key > s->key) - (f->key < s->key);
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64map_compile(char* db_place, size_t db_place_size,
                                      hm_u64map_database_t** db_ptr,
                                      const uint64_t* keys,
                                      const uint64_t* values,
                                      unsigned int elements) {
  if (elements == 0) {
    return HM_ERROR_NO_MASKS;
  }

  // Make sure 0 is not among the keys and the values. We use 0 for empty
  // buckets and return it from hm_u64map_find indicating a missing element, so
  // we can't guarantee correctness if one of the keys or values is 0.
  for (int i = 0; i < elements; i++) {
    if (keys[i] == 0 || values[i] == 0) {
      return HM_ERROR_BAD_VALUE;
    }
  }

  // Align db_place forward, if needed.
  {
    char* db_place2 = align64(db_place);
    db_place_size -= (db_place2 - db_place);
    db_place = db_place2;
  }

  if (db_place_size < hm_u64map_db_place_size(elements) - alignment) {
    return HM_ERROR_SMALL_PLACE;
  }

  int buckets = hash_table_buckets(elements);

  // Fill database struct and db_ptr.
  hm_u64map_database_t* db = (hm_u64map_database_t*)(db_place);
  db->mask_for_hash = buckets - 1 - 3;
  *db_ptr = db;
  db_place += sizeof(hm_u64map_database_t);

  db->hash_table = (key_value_t*)(db_place);
  db_place += sizeof(key_value_t) * buckets;
  key_value_t* hash_table_end = (key_value_t*)(db_place);

  // Initiate the hash function with some random values.
  db->factor1 = 0xA6C3096657A14E89;
  db->factor2 = 0x24F963569D05D92E;

  // In the hash table, buckets form groups of size 4. Bucket i, i+1, i+2, and
  // i+3 store elements from the same set. This helps to deal with collisions to
  // some extent.

  // Find factor1 and factor2 not resulting in hash collisions overflowing
  // grouped buckets. At the same time check that all the elements are unique.
  while (true) {
    // Clear the hash table.
    clear_hash_table(db);

    // Put keys into the buckets, until encounting a collision.
    bool collision = false;
    for (int i = 0; i < elements; i++) {
      uint64_t key = keys[i];
      uint64_t value = values[i];
      uint64_t h = hm_u64map_hash64(db, key);
      uint64_t b = h & db->mask_for_hash;
      if (db->hash_table[b].key == key || db->hash_table[b + 1].key == key ||
          db->hash_table[b + 2].key == key ||
          db->hash_table[b + 3].key == key) {
        // Non-uniqueness.
        return HM_ERROR_BAD_VALUE;
      } else if (db->hash_table[b].key == 0) {
        db->hash_table[b].key = key;
        db->hash_table[b].value = value;
        debugf("put (0x%" PRIx64 ", 0x%" PRIx64 ") to cell %" PRIu64 "\n", key,
               value, b);
      } else if (db->hash_table[b + 1].key == 0) {
        db->hash_table[b + 1].key = key;
        db->hash_table[b + 1].value = value;
        debugf("put (0x%" PRIx64 ", 0x%" PRIx64 ") to cell %" PRIu64 "\n", key,
               value, b + 1);
      } else if (db->hash_table[b + 2].key == 0) {
        db->hash_table[b + 2].key = key;
        db->hash_table[b + 2].value = value;
        debugf("put (0x%" PRIx64 ", 0x%" PRIx64 ") to cell %" PRIu64 "\n", key,
               value, b + 2);
      } else if (db->hash_table[b + 3].key == 0) {
        db->hash_table[b + 3].key = key;
        db->hash_table[b + 3].value = value;
        debugf("put (0x%" PRIx64 ", 0x%" PRIx64 ") to cell %" PRIu64 "\n", key,
               value, b + 3);
      } else {
        collision = true;
        debugf("Collision! Rebuilding the table with new hash function.\n");
        break;
      }
    }

    if (!collision) {
      // Great! We found values of factor1 and factor2 for which our keys
      // do not produce any collisions. Now fill the table with keys. But
      // first clear the hash table.
      break;
    }

    // Change factors of the hash function.
    db->factor1 = hm_u64map_hash64(db, keys[0]);
    db->factor2 = hm_u64map_hash64(db, keys[0]);
  }

  // Now we should change the value stored in the bucket to which 0 maps
  // to some value which is not 0 and not mapped there (not to create a false
  // positive).
  uint64_t h0 = hm_u64map_hash64(db, 0);
  uint64_t b = h0 & db->mask_for_hash;
  for (uint64_t shift = 0; shift < items_in_bucket; shift++) {
    uint64_t b0 = b + shift;
    if (db->hash_table[b0].key == 0) {
      while (true) {
        db->hash_table[b0].key++;
        uint64_t h1 = hm_u64map_hash64(db, db->hash_table[b0].key);
        uint64_t b1 = h1 & db->mask_for_hash;
        if (b1 != (b0 & db->mask_for_hash)) {
          break;
        }
      }
    }
  }

  // Sort values insude buckets fot speed.
  for (key_value_t* start = db->hash_table; start < hash_table_end;
       start += items_in_bucket) {
    qsort(start, items_in_bucket, sizeof(key_value_t), comp_key_value);
  }

  debugf("compile factors: %" PRIu64 " %" PRIu64 "\n", db->factor1,
         db->factor2);
  debugf("hash_table: %p\n", db->hash_table);

  return HM_SUCCESS;
}

HM_PUBLIC_API
uint64_t HM_CDECL hm_u64map_find(const hm_u64map_database_t* db,
                                 const uint64_t key) {
  uint64_t h = hm_u64map_hash64(db, key);
  uint64_t b = h & db->mask_for_hash;

  debugf("hm_u64map_find: key=0x%" PRIx64 ", h=0x%" PRIx64 ", b=%" PRIu64
         ", hash_table[%" PRIu64 "]={0x%" PRIx64 ", 0x%" PRIx64
         "}, hash_table[%" PRIu64 "]={0x%" PRIx64 ", 0x%" PRIx64
         "}, hash_table[%" PRIu64 "]={0x%" PRIx64 ", 0x%" PRIx64
         "}, hash_table[%" PRIu64 "]={0x%" PRIx64 ", 0x%" PRIx64 "}\n",
         key, h, b, b, db->hash_table[b].key, db->hash_table[b].value, b + 1,
         db->hash_table[b + 1].key, db->hash_table[b + 1].value, b + 2,
         db->hash_table[b + 2].key, db->hash_table[b + 2].value, b + 3,
         db->hash_table[b + 3].key, db->hash_table[b + 3].value);

  return db->hash_table[b].key == key       ? db->hash_table[b].value
         : db->hash_table[b + 1].key == key ? db->hash_table[b + 1].value
         : db->hash_table[b + 2].key == key ? db->hash_table[b + 2].value
         : db->hash_table[b + 3].key == key ? db->hash_table[b + 3].value
                                            : 0;
}

HM_PUBLIC_API
uint64_t HM_CDECL hm_u64map_benchmark(const hm_u64map_database_t* db,
                                      uint64_t begin_key, uint64_t end_key) {
  uint64_t xor_sum = 0;
  for (uint64_t key = begin_key; key != end_key; key++) {
    xor_sum ^= hm_u64map_find(db, key);
  }

  return xor_sum;
}

// Serialized form:
// uint64_t factor1
// uint64_t factor2
// uint64_t buckets
// uint64_t 0 (dummy)
// []key_value_t hash_table

HM_PUBLIC_API
size_t HM_CDECL hm_u64map_serialized_size(const hm_u64map_database_t* db) {
  return 4 * sizeof(uint64_t) + get_buckets(db) * sizeof(key_value_t);
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64map_serialize(char* buffer, size_t buffer_size,
                                        const hm_u64map_database_t* db) {
  if (buffer_size < hm_u64map_serialized_size(db)) {
    return HM_ERROR_SMALL_PLACE;
  }

  uint64_t buckets = get_buckets(db);

  uint64_t* dst = (uint64_t*)(buffer);
  *dst = db->factor1;
  dst++;
  *dst = db->factor2;
  dst++;
  *dst = buckets;

  buffer += 4 * sizeof(uint64_t);

  key_value_t* hash_table2 = (key_value_t*)(buffer);
  for (int i = 0; i < buckets; i++) {
    hash_table2[i] = db->hash_table[i];
  }

  return HM_SUCCESS;
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64map_db_place_size_from_serialized(
    size_t* db_place_size, const char* buffer, size_t buffer_size) {
  if (buffer_size <= 4 * sizeof(uint64_t)) {
    return HM_ERROR_SMALL_PLACE;
  }

  const uint64_t* src = (const uint64_t*)(buffer);

  // Skip factor1 and factor2.
  src++;
  src++;

  uint64_t buckets = *src;

  if (buckets == 0) {
    return HM_ERROR_NO_MASKS;
  }

  size_t min_buffer_size = 3 * sizeof(uint64_t) + buckets * sizeof(key_value_t);
  if (buffer_size < min_buffer_size) {
    return HM_ERROR_SMALL_PLACE;
  }

  *db_place_size = get_db_place(buckets);

  return HM_SUCCESS;
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64map_deserialize(char* db_place, size_t db_place_size,
                                          hm_u64map_database_t** db_ptr,
                                          const char* buffer,
                                          size_t buffer_size) {
  size_t min_db_place_size;
  hm_error_t err = hm_u64map_db_place_size_from_serialized(&min_db_place_size,
                                                           buffer, buffer_size);
  if (err != HM_SUCCESS) {
    return err;
  }

  // Align db_place forward, if needed.
  {
    char* db_place2 = align64(db_place);
    db_place_size -= (db_place2 - db_place);
    db_place = db_place2;
  }

  if (db_place_size < min_db_place_size - alignment) {
    return HM_ERROR_SMALL_PLACE;
  }

  const uint64_t* src = (const uint64_t*)(buffer);
  hm_u64map_database_t* db = (hm_u64map_database_t*)(db_place);
  *db_ptr = db;
  db->factor1 = *src;
  src++;
  db->factor2 = *src;
  src++;
  uint64_t buckets = *src;
  db->mask_for_hash = buckets - 1 - 3;

  buffer += 4 * sizeof(uint64_t);
  db_place += sizeof(hm_u64map_database_t);

  db->hash_table = (key_value_t*)(db_place);

  const key_value_t* hash_table0 = (const key_value_t*)(buffer);
  for (int i = 0; i < buckets; i++) {
    db->hash_table[i] = hash_table0[i];
  }

  debugf("factors: %" PRIu64 " %" PRIu64 "\n", db->factor1, db->factor2);

  debugf("hash_table: %p\n", db->hash_table);

  return HM_SUCCESS;
}
