#include <assert.h>
#include <stdio.h>

#include "static_uint64_set.h"

#ifdef NDEBUG
#define debugf(fmt, ...)                                                       \
  do {                                                                         \
  } while (0)
#else
#define debugf printf
#endif

// Alignment is for 4 uint64's to be in one cache line.
static const size_t alignment = 32;

typedef struct hm_u64_database {
  // Hash table. Elements of the array are uint64 keys.
  uint64_t *hash_table;

  // Factors for multiplication in hm_u64_hash64.
  uint64_t factor1, factor2;

  // Mask to go from hash64 to bucket index.
  uint64_t mask_for_hash;
} hm_u64_database_t;

// https://stackoverflow.com/a/6867612
static uint64_t hm_u64_hash64(const hm_u64_database_t *db, uint64_t key) {
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

static inline char *align64(char *addr) {
  return (char *)(((uintptr_t)(addr) & ~(alignment - 1)) + alignment);
}

static inline int hash_table_buckets(unsigned int elements) {
  // Calibrate the number of buckets to make probability of having a 5-way
  // collision less than 1 (e.g. 99.5%), so the number of attempts in
  // compilation is not too high. The probability of having a 5-way collision
  // among N elements put into M 4-bucket groups is 1 - exp(-(N choose 5) /
  // M^4). For 10k elements the factor of 2 works well.

  int result = round_up_to_power_of_2(elements) * 4 * 2;
  if (result < 16) {
    result = 16;
  }

  return result;
}

static inline uint64_t get_buckets(const hm_u64_database_t *db) {
  return db->mask_for_hash + 1 + 3;
}

static inline void clear_hash_table(hm_u64_database_t *db) {
  uint64_t buckets = get_buckets(db);
  for (int i = 0; i < buckets; i++) {
    db->hash_table[i] = 0;
  }
}

static inline size_t get_db_place(int buckets) {
  return sizeof(hm_u64_database_t) + buckets * sizeof(uint64_t) + alignment;
}

HM_PUBLIC_API
size_t HM_CDECL hm_u64_db_place_size(unsigned int elements) {
  return get_db_place(hash_table_buckets(elements));
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64_compile(char *db_place, size_t db_place_size,
                                   hm_u64_database_t **db_ptr,
                                   const uint64_t *keys,
                                   unsigned int elements) {
  if (elements == 0) {
    return HM_ERROR_NO_MASKS;
  }

  // Make sure 0 is not among the keys. We use 0 for empty buckets, so
  // we can't guarantee correctness if one of the keys is 0.
  for (int i = 0; i < elements; i++) {
    if (keys[i] == 0) {
      return HM_ERROR_BAD_VALUE;
    }
  }

  // Align db_place forward, if needed.
  {
    char *db_place2 = align64(db_place);
    db_place_size -= (db_place2 - db_place);
    db_place = db_place2;
  }

  if (db_place_size < hm_u64_db_place_size(elements) - alignment) {
    return HM_ERROR_SMALL_PLACE;
  }

  int buckets = hash_table_buckets(elements);

  // Fill database struct and db_ptr.
  hm_u64_database_t *db = (hm_u64_database_t *)(db_place);
  db->mask_for_hash = buckets - 1 - 3;
  *db_ptr = db;
  db_place += sizeof(hm_u64_database_t);

  db->hash_table = (uint64_t *)(db_place);
  db_place += sizeof(uint64_t) * buckets;

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
      uint64_t h = hm_u64_hash64(db, key);
      uint64_t b = h & db->mask_for_hash;
      if (db->hash_table[b] == key || db->hash_table[b + 1] == key ||
          db->hash_table[b + 2] == key || db->hash_table[b + 3] == key) {
        // Non-uniqueness.
        return HM_ERROR_BAD_VALUE;
      } else if (db->hash_table[b] == 0) {
        db->hash_table[b] = key;
      } else if (db->hash_table[b + 1] == 0) {
        db->hash_table[b + 1] = key;
      } else if (db->hash_table[b + 2] == 0) {
        db->hash_table[b + 2] = key;
      } else if (db->hash_table[b + 3] == 0) {
        db->hash_table[b + 3] = key;
      } else {
        collision = true;
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
    db->factor1 = hm_u64_hash64(db, keys[0]);
    db->factor2 = hm_u64_hash64(db, keys[0]);
  }

  // Now we should change the value stored in the bucket to which 0 maps
  // to some value which is not 0 and not mapped there (not to create a false
  // positive).
  uint64_t h0 = hm_u64_hash64(db, 0);
  uint64_t b = h0 & db->mask_for_hash;
  for (uint64_t shift = 0; shift < 4; shift++) {
    uint64_t b0 = b + shift;
    if (db->hash_table[b0] == 0) {
      while (true) {
        db->hash_table[b0]++;
        uint64_t h1 = hm_u64_hash64(db, db->hash_table[b0]);
        uint64_t b1 = h1 & db->mask_for_hash;
        if (b1 != (b0 & db->mask_for_hash)) {
          break;
        }
      }
    }
  }

  debugf("compile factors: %d %d\n", db->factor1, db->factor2);
  debugf("hash_table: %p\n", db->hash_table);

  return HM_SUCCESS;
}

HM_PUBLIC_API
bool HM_CDECL hm_u64_find(const hm_u64_database_t *db, const uint64_t key) {
  uint64_t h = hm_u64_hash64(db, key);
  uint64_t b = h & db->mask_for_hash;
  return db->hash_table[b] == key || db->hash_table[b + 1] == key ||
         db->hash_table[b + 2] == key || db->hash_table[b + 3] == key;
}

HM_PUBLIC_API
uint64_t HM_CDECL hm_u64_benchmark(const hm_u64_database_t *db,
                                   uint64_t begin_key, uint64_t end_key) {
  uint64_t count = 0;
  for (uint64_t key = begin_key; key != end_key; key++) {
    count += (uint64_t)(hm_u64_find(db, key));
  }

  return count;
}

// Serialized form: factor1, factor2, buckets, then hash_table.

HM_PUBLIC_API
size_t HM_CDECL hm_u64_serialized_size(const hm_u64_database_t *db) {
  return (3 + get_buckets(db)) * sizeof(uint64_t);
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64_serialize(char *buffer, size_t buffer_size,
                                     const hm_u64_database_t *db) {
  if (buffer_size < hm_u64_serialized_size(db)) {
    return HM_ERROR_SMALL_PLACE;
  }

  uint64_t buckets = get_buckets(db);

  uint64_t *dst = (uint64_t *)(buffer);
  *dst = db->factor1;
  dst++;
  *dst = db->factor2;
  dst++;
  *dst = buckets;

  buffer += 3 * sizeof(uint64_t);

  uint64_t *hash_table2 = (uint64_t *)(buffer);
  for (int i = 0; i < buckets; i++) {
    hash_table2[i] = db->hash_table[i];
  }

  return HM_SUCCESS;
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64_db_place_size_from_serialized(size_t *db_place_size,
                                                         const char *buffer,
                                                         size_t buffer_size) {
  if (buffer_size <= 3 * sizeof(uint64_t)) {
    return HM_ERROR_SMALL_PLACE;
  }

  const uint64_t *src = (const uint64_t *)(buffer);

  // Skip factor1 and factor2.
  src++;
  src++;

  uint64_t buckets = *src;

  if (buckets == 0) {
    return HM_ERROR_NO_MASKS;
  }

  size_t min_buffer_size = (3 + buckets) * sizeof(uint64_t);
  if (buffer_size < min_buffer_size) {
    return HM_ERROR_SMALL_PLACE;
  }

  *db_place_size = get_db_place(buckets);

  return HM_SUCCESS;
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_u64_deserialize(char *db_place, size_t db_place_size,
                                       hm_u64_database_t **db_ptr,
                                       const char *buffer, size_t buffer_size) {
  size_t min_db_place_size;
  hm_error_t err = hm_u64_db_place_size_from_serialized(&min_db_place_size,
                                                        buffer, buffer_size);
  if (err != HM_SUCCESS) {
    return err;
  }

  // Align db_place forward, if needed.
  {
    char *db_place2 = align64(db_place);
    db_place_size -= (db_place2 - db_place);
    db_place = db_place2;
  }

  if (db_place_size < min_db_place_size - alignment) {
    return HM_ERROR_SMALL_PLACE;
  }

  const uint64_t *src = (const uint64_t *)(buffer);
  hm_u64_database_t *db = (hm_u64_database_t *)(db_place);
  *db_ptr = db;
  db->factor1 = *src;
  src++;
  db->factor2 = *src;
  src++;
  uint64_t buckets = *src;
  db->mask_for_hash = buckets - 1 - 3;

  buffer += 3 * sizeof(uint64_t);
  db_place += sizeof(hm_u64_database_t);

  db->hash_table = (uint64_t *)(db_place);

  const uint64_t *hash_table0 = (const uint64_t *)(buffer);
  for (int i = 0; i < buckets; i++) {
    db->hash_table[i] = hash_table0[i];
  }

  debugf("factors: %d %d\n", db->factor1, db->factor2);

  debugf("hash_table: %p\n", db->hash_table);

  return HM_SUCCESS;
}
