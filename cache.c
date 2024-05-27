#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cache.h"

// Order in the linked list: from newest to oldest!
// Element at index 0 is a special element, it does not store
// any real element, but serves as previous element for
// the newest element. If its next_index field is 0,
// the list is empty.

// Free elements in the list are linked as another list.
// The first element of the list is stored in first_free_index.
// The last element of the list has next_index=0.
// If first_free_index=0, the list is fully filled.

// If oldest_index=0, the list is empty.

static const size_t alignment = 8;
static const uint32_t empty_bucket_value = 0xFFFFFFFF;
static const int max_pushes = 100;
static const uint64_t random1 = 0xA6C3096657A14E89;
static const uint64_t random2 = 0x24F963569D05D92E;

// https://stackoverflow.com/a/6867612
static uint64_t hm_hash64(uint64_t key) {
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccd;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53;
  key ^= key >> 33;
  return key;
}

typedef struct hm_cache_element {
  uint32_t ip;
  uint32_t next_index;
} hm_cache_element_t;

typedef struct hm_cache {
  uint64_t salt;
  uint64_t bits;          // Bits in hashes returned by hm_two_buckets.
  uint64_t mask_for_hash; // 'bits' lower bits 1, other bits 0.

  hm_cache_element_t *list;
  uint32_t *values;

  // Hash table. Key is IP. Value is index of previous element.
  uint32_t *ip2prev;

  uint32_t first_free_index;
  uint32_t oldest_index;
} hm_cache_t;

static inline bool hm_valid_capacity(unsigned int capacity) {
  // Make sure it is a power of 2 and at least 2.
  return capacity >= 2 && (capacity & (capacity - 1)) == 0;
}

// x must be a power of 2.
static inline uint64_t log2int(uint64_t x) {
  uint64_t result = 0;
  while (x != 1) {
    x = x >> 1;
    result++;
    assert(x != 0);
  }
  return result;
}

static inline void hm_two_buckets(hm_cache_t *cache, uint32_t *bucket1,
                                  uint32_t *bucket2, const uint32_t ip) {
  uint64_t h = hm_hash64(cache->salt ^ ip);
  *bucket1 = h & cache->mask_for_hash;
  *bucket2 = (h >> cache->bits) & cache->mask_for_hash;
}

static inline bool hm_cache_locate(hm_cache_t *cache, uint32_t ip,
                                   uint32_t **bucket, uint32_t **another_bucket,
                                   uint32_t *prev_ptr, uint32_t *index_ptr) {

  uint32_t b1, b2;
  hm_two_buckets(cache, &b1, &b2, ip);

  {
    // Try the first bucket.
    uint32_t prev = cache->ip2prev[b1];
    if (prev != empty_bucket_value) {
      uint32_t index = cache->list[prev].next_index;
      if (cache->list[index].ip == ip) {
        // Match in the first bucket.
        *bucket = &(cache->ip2prev[b1]);
        *another_bucket = &(cache->ip2prev[b2]);
        *prev_ptr = prev;
        *index_ptr = index;
        return true;
      }
    }
  }

  {
    uint32_t prev = cache->ip2prev[b2];
    if (prev != empty_bucket_value) {
      uint32_t index = cache->list[prev].next_index;
      if (cache->list[index].ip == ip) {
        // Match in the second bucket.
        *bucket = &(cache->ip2prev[b2]);
        *another_bucket = &(cache->ip2prev[b1]);
        *prev_ptr = prev;
        *index_ptr = index;
        return true;
      }
    }
  }

  *bucket = &(cache->ip2prev[b1]);
  *another_bucket = &(cache->ip2prev[b2]);

  return false;
}

static inline void hm_update_prev(hm_cache_t *cache, uint32_t prev,
                                  uint32_t next) {

  uint32_t next_ip = cache->list[next].ip;

  uint32_t *bucket, *another_bucket;
  uint32_t next_prev, index;
  bool has = hm_cache_locate(cache, next_ip, &bucket, &another_bucket,
                             &next_prev, &index);
  assert(has);

  *bucket = prev;
}

static inline void hm_set_newest(hm_cache_t *cache, uint32_t *bucket,
                                 uint32_t prev, uint32_t index) {
  uint32_t next = cache->list[index].next_index;

  // Move to the beginning of the list.
  cache->list[prev].next_index = next;
  cache->list[index].next_index = cache->list[0].next_index;
  cache->list[0].next_index = index;
  *bucket = 0;

  if (next == 0) {
    // The current element was the oldest. Now prev is the oldest.
    cache->oldest_index = prev;
  } else {
    // If next element exists, its prev value changes from index to prev.
    hm_update_prev(cache, prev, next);
  }
}

static inline uint32_t *find_another_bucket(hm_cache_t *cache,
                                            uint32_t *bucket) {
  // Find the IP using this bucket.
  uint32_t prev = *bucket;
  uint32_t index = cache->list[prev].next_index;
  uint32_t ip = cache->list[index].ip;

  // Find another possible bucket of the IP.
  uint32_t b1, b2;
  hm_two_buckets(cache, &b1, &b2, ip);
  uint32_t *bucket1 = &(cache->ip2prev[b1]);
  uint32_t *bucket2 = &(cache->ip2prev[b2]);
  assert(bucket == bucket1 || bucket == bucket2);

  return (bucket == bucket1) ? bucket2 : bucket1;
}

// Try to free the bucket by pushing current value to another slot (Cuckoo
// hashing). If another slot is also occupied, its value is pushed further. The
// process repeats max max_pushes times or failes otherwise.
static inline bool try_push_bucket(hm_cache_t *cache, uint32_t *bucket) {
  uint32_t moved_value = *bucket;
  for (int i = 0; i < max_pushes; i++) {
    uint32_t *another_bucket = find_another_bucket(cache, bucket);
    uint32_t value = *another_bucket;
    *another_bucket = moved_value;
    if (value == empty_bucket_value) {
      return true;
    }
    moved_value = value;
    bucket = another_bucket;
  }

  return false;
}

static inline bool hm_hashtable_try_set(hm_cache_t *cache, uint32_t *bucket,
                                        uint32_t *another_bucket,
                                        const uint32_t ip, uint32_t prev) {
  if (*bucket == empty_bucket_value) {
    *bucket = prev;
    return true;
  }
  if (*another_bucket == empty_bucket_value) {
    *another_bucket = prev;
    return true;
  }

  // Try to push the value from bucket.
  if (try_push_bucket(cache, bucket)) {
    *bucket = prev;
    return true;
  }

  return false;
}

static inline bool hm_try_rehash(hm_cache_t *cache) {
  cache->salt = hm_hash64(cache->salt ^ random2);

  // Fill hash table with empty_bucket_value.
  uint64_t hash_table_capacity = cache->mask_for_hash + 1;
  memset(cache->ip2prev, 0xFF, hash_table_capacity * sizeof(uint32_t));

  uint32_t prev = 0;
  while (true) {
    uint32_t index = cache->list[prev].next_index;
    if (index == 0) {
      break;
    }
    uint32_t ip = cache->list[index].ip;

    uint32_t b1, b2;
    hm_two_buckets(cache, &b1, &b2, ip);
    uint32_t *bucket = &(cache->ip2prev[b1]);
    uint32_t *another_bucket = &(cache->ip2prev[b2]);
    if (!hm_hashtable_try_set(cache, bucket, another_bucket, ip, prev)) {
      return false;
    }

    prev = index;
  }
  return true;
}

static inline void hm_rehash(hm_cache_t *cache) {
  while (!hm_try_rehash(cache)) {
  }
}

static inline uint64_t get_hash_table_capacity(uint64_t capacity) {
  return capacity << 1;
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_cache_place_size(size_t *cache_place_size,
                                        unsigned int capacity) {
  if (!hm_valid_capacity(capacity)) {
    return HM_ERROR_BAD_SIZE;
  }
  assert(sizeof(hm_cache_t) % alignment == 0);

  // Cuckoo hash table needs 2x capacity.
  uint64_t hash_table_capacity = get_hash_table_capacity(capacity);

  *cache_place_size =
      sizeof(hm_cache_t) + sizeof(hm_cache_element_t) * capacity +
      sizeof(uint32_t) * capacity + hash_table_capacity * sizeof(uint32_t);

  return HM_SUCCESS;
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_cache_init(char *cache_place, size_t cache_place_size,
                                  hm_cache_t **cache_ptr,
                                  unsigned int capacity) {
  // Check alignment.
  if ((uintptr_t)(cache_place) % alignment != 0) {
    return HM_ERROR_BAD_ALIGNMENT;
  }

  size_t cache_place_needed;
  hm_error_t hm_err = hm_cache_place_size(&cache_place_needed, capacity);
  if (hm_err != HM_SUCCESS) {
    return hm_err;
  }
  if (cache_place_size < cache_place_needed) {
    return HM_ERROR_SMALL_PLACE;
  }

  // Zero all the memory.
  memset(cache_place, 0, cache_place_size);

  // Cuckoo hash table needs 2x capacity.
  uint64_t hash_table_capacity = get_hash_table_capacity(capacity);

  hm_cache_t *cache = (hm_cache_t *)(cache_place);
  *cache_ptr = cache;
  cache_place += sizeof(hm_cache_t);

  cache->list = (hm_cache_element_t *)(cache_place);
  cache_place += sizeof(hm_cache_element_t) * capacity;

  cache->values = (uint32_t *)(cache_place);
  cache_place += sizeof(uint32_t) * capacity;

  cache->ip2prev = (uint32_t *)(cache_place);
  cache_place += sizeof(uint32_t) * hash_table_capacity;

  cache->oldest_index = 0;

  // Fill hash table with empty_bucket_value.
  memset(cache->ip2prev, 0xFF, hash_table_capacity * sizeof(uint32_t));

  // Set next index in 0 to 0 (the list is empty).
  cache->list[0].next_index = 0;

  // Turn the list into free-list.
  cache->first_free_index = 1;
  for (int i = 1; i <= capacity - 2; i++) {
    cache->list[i].next_index = i + 1;
  }
  cache->list[capacity - 1].next_index = 0;

  cache->salt = random1;

  cache->bits = log2int(hash_table_capacity);
  cache->mask_for_hash = hash_table_capacity - 1;

  return HM_SUCCESS;
}

HM_PUBLIC_API
void HM_CDECL hm_cache_add(hm_cache_t *cache, const uint32_t ip,
                           const uint32_t value, bool *existed, bool *evicted,
                           uint32_t *evicted_ip, uint32_t *evicted_value) {
  uint32_t *bucket, *another_bucket;
  uint32_t prev, index;
  if (hm_cache_locate(cache, ip, &bucket, &another_bucket, &prev, &index)) {
    // The IP is already in cache.
    // Update the value and make the newest in the list.
    hm_set_newest(cache, bucket, prev, index);
    cache->values[index] = value;
    *existed = true;
    *evicted = false;
    return;
  }

  *existed = false;

  // Find the place in the linked list.
  if (cache->first_free_index == 0) {
    // The list is fully filled.
    index = cache->oldest_index;
    uint32_t ip_to_evict = cache->list[index].ip;

    // Remove old element from the hash table.
    uint32_t *old_bucket, *old_another_bucket;
    uint32_t old_prev, old_index;
    bool had = hm_cache_locate(cache, ip_to_evict, &old_bucket,
                               &old_another_bucket, &old_prev, &old_index);
    assert(had);
    *bucket = empty_bucket_value;

    *evicted = true;
    *evicted_ip = ip_to_evict;
    *evicted_value = cache->values[index];

    // The table stores previous index, so it is
    // what we need - previous element of the oldest.
    cache->oldest_index = old_prev;
  } else {
    index = cache->first_free_index;
    cache->first_free_index = cache->list[index].next_index;

    *evicted = false;
  }

  // There is no previous element of the newest element.
  // The record in the hash table for the IP points to list element 0.
  cache->list[index].next_index = cache->list[0].next_index;
  cache->list[0].next_index = index;

  // Put IP and value at index.
  cache->list[index].ip = ip;
  cache->values[index] = value;

  // Find an empty bucket in the hash table and set prev=0 for IP.
  // May involve pushing elements around.
  if (hm_hashtable_try_set(cache, bucket, another_bucket, ip, 0)) {
    return;
  }

  // Failed to push elements to find place for every element.

  // Rehash the whole table using the data from the list.
  // The value (prev) will be set in the process, because it is
  // already in the list.
  hm_rehash(cache);
}

HM_PUBLIC_API
void HM_CDECL hm_cache_remove(hm_cache_t *cache, const uint32_t ip,
                              bool *existed, uint32_t *existed_value) {
  uint32_t *bucket, *another_bucket;
  uint32_t prev, index;
  if (!hm_cache_locate(cache, ip, &bucket, &another_bucket, &prev, &index)) {
    // The IP not in the hash table.
    *existed = false;
    return;
  }

  // Real element can not be at index 0.
  assert(index != 0);

  // Set results.
  *existed = true;
  *existed_value = cache->values[index];

  // Set the value in the hash table to 0.
  // Element 0 in the list serves as previous element, so the check
  // in hm_cache_has won't pass: in an empty list 0's next is also 0,
  // otherwise it is the first element, which has another IP.
  *bucket = 0;

  // Set prev's next to current's next (cut the element from the list).
  uint32_t next = cache->list[index].next_index;
  cache->list[prev].next_index = next;

  // Update free list.
  cache->list[index].next_index = cache->first_free_index;
  cache->first_free_index = index;

  if (next == 0) {
    // The current element was the oldest. Now prev is the oldest.
    cache->oldest_index = prev;
  } else {
    // If next element exists, its prev value changes from index to prev.
    hm_update_prev(cache, prev, next);
  }
}

HM_PUBLIC_API
bool HM_CDECL hm_cache_has(hm_cache_t *cache, const uint32_t ip) {
  uint32_t *bucket, *another_bucket;
  uint32_t prev, index;
  if (!hm_cache_locate(cache, ip, &bucket, &another_bucket, &prev, &index)) {
    return false;
  }

  assert(index != 0);

  hm_set_newest(cache, bucket, prev, index);

  return true;
}
