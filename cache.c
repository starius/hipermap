#include "cache.h"

#include <assert.h>
#include <stdio.h>

#ifdef NDEBUG
#define debugf(fmt, ...) \
  do {                   \
  } while (0)
#else
#define debugf printf
#endif

// Order in the linked list: from newest to oldest!

// Element at index -1 is not used. It index serves as NULL pointer (NO_INDEX).
#define NO_INDEX (0xFFFFFFFF)

static const size_t alignment = 8;

typedef struct hm_cache_element {
  uint32_t ip;
  uint32_t prev_index;
  uint32_t next_index;
  uint32_t value;
} hm_cache_element_t;

typedef struct hm_list {
  uint32_t head_index;
  uint32_t tail_index;
} hm_list_t;

typedef struct hm_cache {
  // Mask to go from hash32 to bucket index.
  uint32_t mask_for_hash;

  uint32_t capacity;

  // list_storage is an array used to store elements of both the list and the
  // free list. Each element belongs either to the list or to the free list.
  hm_cache_element_t* list_storage;

  // nodes is the list.
  hm_list_t nodes;

  // free_nodes is the free list.
  hm_list_t free_nodes;

  // Hash table. Key is IP. Value is index of previous element.
  uint32_t* hash_table;
} hm_cache_t;

static inline void cut(hm_cache_element_t* list_storage, hm_list_t* list,
                       uint32_t index) {
  hm_cache_element_t* e = &(list_storage[index]);
  uint32_t prev_index = e->prev_index;
  uint32_t next_index = e->next_index;
  e->prev_index = NO_INDEX;
  e->next_index = NO_INDEX;
  if (prev_index != NO_INDEX && next_index != NO_INDEX) {
    hm_cache_element_t* prev = &(list_storage[prev_index]);
    hm_cache_element_t* next = &(list_storage[next_index]);
    prev->next_index = next_index;
    next->prev_index = prev_index;
  } else if (prev_index != NO_INDEX) {
    // The element is the oldest element.
    hm_cache_element_t* prev = &(list_storage[prev_index]);
    prev->next_index = NO_INDEX;
    list->tail_index = prev_index;
  } else if (next_index != NO_INDEX) {
    // The element is the newest element.
    hm_cache_element_t* next = &(list_storage[next_index]);
    next->prev_index = NO_INDEX;
    list->head_index = next_index;
  } else {
    // The element is the only element.
    list->head_index = NO_INDEX;
    list->tail_index = NO_INDEX;
  }
}

static inline void set_head(hm_cache_element_t* list_storage, hm_list_t* list,
                            uint32_t index) {
  if (list->head_index == NO_INDEX) {
    list->head_index = index;
    list->tail_index = index;
  } else {
    hm_cache_element_t* e = &(list_storage[index]);
    hm_cache_element_t* head = &(list_storage[list->head_index]);
    e->next_index = list->head_index;
    head->prev_index = index;
    list->head_index = index;
  }
}

// Fast hash function on uint32_t.
// See
// https://www.reddit.com/r/C_Programming/comments/vv8yql/weird_problem_print_every_32_bit_number_once_in/?rdt=35253
// See https://github.com/skeeto/hash-prospector/issues/19
static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x21f0aaad;
  x ^= x >> 15;
  x *= 0xd35a2d97;
  x ^= x >> 15;
  return x;
}

// table_lookup looks for ip in the table and returns its indes or NO_INDEX.
static inline uint32_t table_lookup(hm_cache_t* cache, uint32_t ip) {
  // Cache some variables.
  uint32_t mask = cache->mask_for_hash;
  uint32_t* hash_table = cache->hash_table;
  hm_cache_element_t* list_storage = cache->list_storage;

  // Find the start bucket.
  uint32_t bucket = hash32(ip) & mask;

  // Iterate until the key is found or an empty bucket is found.
  // This loop has to stop, because the table always has >=50% empty cells.
  while (true) {
    uint32_t index = hash_table[bucket];
    if (index == NO_INDEX) {
      return NO_INDEX;
    }
    if (list_storage[index].ip == ip) {
      return index;
    }

    // Go to next bucket.
    bucket++;
    bucket &= mask;
  }
}

// table_add assigns the index as a value for the ip. The IP must not exist in
// the table before the call.
static inline void table_add(hm_cache_t* cache, uint32_t ip, uint32_t index) {
  // Cache some variables.
  uint32_t mask = cache->mask_for_hash;
  uint32_t* hash_table = cache->hash_table;
  hm_cache_element_t* list_storage = cache->list_storage;

  // Make sure the IP is set in the list element.
  assert(list_storage[index].ip == ip);

  // Find the start bucket.
  uint32_t bucket = hash32(ip) & mask;

  debugf("table_add: ip=%d index=%d start_bucket=%d\n", ip, index, bucket);

  // Iterate until an empty bucket is found.
  // This loop has to stop, because the table always has >=50% empty cells.
  while (true) {
    uint32_t old_index = hash_table[bucket];
    if (old_index == NO_INDEX) {
      hash_table[bucket] = index;

      debugf("Assigned: ip=%d bucket=%d index=%d\n", ip, bucket, index);
      return;
    }

    // Make sure the IP did not exist before.
    assert(list_storage[old_index].ip != ip);

    // Go to next bucket.
    bucket++;
    bucket &= mask;
  }
}

// table_delete deletes the ip from the table. The IP must exist.
static inline void table_delete(hm_cache_t* cache, uint32_t ip) {
  // Cache some variables.
  uint32_t mask = cache->mask_for_hash;
  uint32_t* hash_table = cache->hash_table;
  hm_cache_element_t* list_storage = cache->list_storage;

  // Find the start bucket.
  uint32_t i = hash32(ip) & mask;

  debugf("table_delete: ip=%d start_bucket=%d\n", ip, i);

  // Iterate until the key is found.
  // This loop has to stop, because the table always has >=50% empty cells.
  while (true) {
    uint32_t old_index = hash_table[i];

    // Make sure we haven't reached empty buckets.
    // This would mean that the element is not found.
    assert(old_index != NO_INDEX);

    if (list_storage[old_index].ip == ip) {
      // We found the element. Stop the loop.
      debugf("Deleted: ip=%d bucket=%d index=%d\n", ip, i, old_index);
      hash_table[i] = NO_INDEX;
      break;
    }

    // Go to next bucket.
    i++;
    i &= mask;
  }

  // For all records in a cluster, there must be no vacant slots between their
  // natural hash position and their current position (else lookups will
  // terminate before finding the record). At this point in the pseudocode,
  // i is a vacant slot that might be invalidating this property for subsequent
  // records in the cluster. j is such a subsequent record. k is the raw hash
  // where the record at j would naturally land in the hash table if there were
  // no collisions. This test is asking if the record at j is invalidly
  // positioned with respect to the required properties of a cluster now that
  // i is vacant.

  uint32_t j = i;
  while (true) {
    j++;
    j &= mask;

    uint32_t j_index = hash_table[j];
    if (j_index == NO_INDEX) {
      break;
    }

    uint32_t k = hash32(list_storage[j_index].ip) & mask;

    // Determine if k lies cyclically in (i,j].
    // i ≤ j: |    i..k..j    |
    // i > j: |.k..j     i....| or |....j     i..k.|
    if (i <= j) {
      if ((i < k) && (k <= j)) {
        continue;
      }
    } else {
      if ((k <= j) || (i < k)) {
        continue;
      }
    }

    hash_table[i] = hash_table[j];
    hash_table[j] = NO_INDEX;
    i = j;
  }
}

// get_hash_table_capacity returns hash table capacity.
// Argument capacity is the maximum number of IPs stored in the cache.
// Speed must be in the range 1-5. The higher the number, the more
// memory is used, but the faster it works.
// How hash table allocation depends on speed:
// speed=1, allocation=50%
// speed=2, allocation=25%
// speed=3, allocation=12.5%
// speed=4, allocation=6.25%
// speed=5, allocation=3.125%
static inline uint64_t get_hash_table_capacity(uint64_t capacity, int speed) {
  return capacity << speed;
}

static inline bool hm_valid_capacity(unsigned int capacity, int speed) {
  // Make sure it is at least 2.
  if (capacity < 2) {
    return false;
  }

  // Make sure it is a power of 2.
  if ((capacity & (capacity - 1)) != 0) {
    return false;
  }

  // Make sure NO_INDEX does not belong to the space of possible indices.
  if (NO_INDEX <= capacity - 1) {
    return false;
  }

  // Make sure get_hash_table_capacity does not overflow.
  if (get_hash_table_capacity(capacity, speed) <= capacity) {
    return false;
  }

  // All the checks are passed.
  return true;
}

static inline char* align8(char* addr) {
  return (char*)(((uintptr_t)(addr) & ~(alignment - 1)) + alignment);
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_cache_place_size(size_t* cache_place_size,
                                        unsigned int capacity, int speed) {
  if (speed < 1 || speed > 5) {
    return HM_ERROR_BAD_SIZE;
  }
  if (!hm_valid_capacity(capacity, speed)) {
    return HM_ERROR_BAD_SIZE;
  }
  assert(sizeof(hm_cache_t) % alignment == 0);

  uint64_t hash_table_capacity = get_hash_table_capacity(capacity, speed);

  *cache_place_size = sizeof(hm_cache_t) +
                      sizeof(hm_cache_element_t) * capacity +
                      hash_table_capacity * sizeof(uint32_t) + alignment;

  return HM_SUCCESS;
}

HM_PUBLIC_API
hm_error_t HM_CDECL hm_cache_init(char* cache_place, size_t cache_place_size,
                                  hm_cache_t** cache_ptr, unsigned int capacity,
                                  int speed) {
  // Align db_place forward, if needed.
  {
    char* cache_place2 = align8(cache_place);
    cache_place_size -= (cache_place2 - cache_place);
    cache_place = cache_place2;
  }

  size_t cache_place_needed;
  hm_error_t hm_err = hm_cache_place_size(&cache_place_needed, capacity, speed);
  if (hm_err != HM_SUCCESS) {
    return hm_err;
  }
  if (cache_place_size < cache_place_needed - alignment) {
    return HM_ERROR_SMALL_PLACE;
  }

  uint64_t hash_table_capacity = get_hash_table_capacity(capacity, speed);

  hm_cache_t* cache = (hm_cache_t*)(cache_place);
  *cache_ptr = cache;
  cache_place += sizeof(hm_cache_t);

  cache->list_storage = (hm_cache_element_t*)(cache_place);
  cache_place += sizeof(hm_cache_element_t) * capacity;

  cache->hash_table = (uint32_t*)(cache_place);
  cache_place += sizeof(uint32_t) * hash_table_capacity;

  // The list is empty.
  cache->nodes.head_index = NO_INDEX;
  cache->nodes.tail_index = NO_INDEX;

  // All the elements belong to free list.
  for (uint32_t i = 0; i < capacity; i++) {
    cache->list_storage[i].prev_index = i - 1;
    cache->list_storage[i].next_index = i + 1;
  }
  cache->free_nodes.head_index = 0;
  cache->free_nodes.tail_index = capacity - 1;
  cache->list_storage[0].prev_index = NO_INDEX;
  cache->list_storage[capacity - 1].next_index = NO_INDEX;

  cache->mask_for_hash = hash_table_capacity - 1;
  cache->capacity = capacity;

  // Put NO_INDEX into all hash_table elements.
  for (uint64_t i = 0; i < hash_table_capacity; i++) {
    cache->hash_table[i] = NO_INDEX;
  }

  return HM_SUCCESS;
}

HM_PUBLIC_API
void HM_CDECL hm_cache_add(hm_cache_t* cache, const uint32_t ip,
                           const uint32_t value, bool* existed, bool* evicted,
                           uint32_t* evicted_ip, uint32_t* evicted_value) {
  // Try to locate the element with this IP in the hash table.
  uint32_t index = table_lookup(cache, ip);
  if (index != NO_INDEX) {
    // The IP is already in cache.

    // Update the value.
    cache->list_storage[index].value = value;

    // Make the newest in the list.
    cut(cache->list_storage, &cache->nodes, index);
    set_head(cache->list_storage, &cache->nodes, index);

    // The element existed before and no element was evicted.
    *existed = true;
    *evicted = false;

    return;
  }

  // The element didn't exist before.
  *existed = false;

  // Find the place in the linked list.
  if (cache->free_nodes.head_index == NO_INDEX) {
    // Free list is empty. Evict some element from the list.
    index = cache->nodes.tail_index;
    assert(index != NO_INDEX);
    uint32_t ip_to_evict = cache->list_storage[index].ip;
    uint32_t value_to_evict = cache->list_storage[index].value;

    assert(ip_to_evict != ip);

    debugf("index to evict: %d, ip_to_evict: %d, value_to_evict: %d\n", index,
           ip_to_evict, value_to_evict);

    // Remove old element from the hash table.
    table_delete(cache, ip_to_evict);

    // An element was evicted, here are its IP and value.
    *evicted = true;
    *evicted_ip = ip_to_evict;
    *evicted_value = value_to_evict;

    // Cut the element from the list.
    cut(cache->list_storage, &cache->nodes, index);
  } else {
    // Use free list is empty. Do not evist anything.
    index = cache->free_nodes.head_index;

    // Cut the element from the free list.
    cut(cache->list_storage, &cache->free_nodes, index);

    // No element was evicted.
    *evicted = false;
  }

  // Insert the element into the list as the newest.
  set_head(cache->list_storage, &cache->nodes, index);

  // Set IP and value for the element.
  cache->list_storage[index].ip = ip;
  cache->list_storage[index].value = value;

  // Set hash table entry.
  table_add(cache, ip, index);
}

HM_PUBLIC_API
void HM_CDECL hm_cache_remove(hm_cache_t* cache, const uint32_t ip,
                              bool* existed, uint32_t* existed_value) {
  // Try to locate the element with this IP in the hash table.
  uint32_t index = table_lookup(cache, ip);
  if (index == NO_INDEX) {
    // The IP not in the hash table.
    *existed = false;
    return;
  }

  // Set results: the element existed, with this value;
  *existed = true;
  *existed_value = cache->list_storage[index].value;

  // Cut the element from the list.
  cut(cache->list_storage, &cache->nodes, index);

  // Add the element to the free list.
  set_head(cache->list_storage, &cache->free_nodes, index);

  // Remove the element from the hash table.
  table_delete(cache, ip);
}

HM_PUBLIC_API
bool HM_CDECL hm_cache_has(hm_cache_t* cache, const uint32_t ip,
                           uint32_t* value) {
  // Try to locate the element with this IP in the hash table.
  uint32_t index = table_lookup(cache, ip);

  if (index == NO_INDEX) {
    // The element is not found in the hash table.
    return false;
  }

  // Make the newest in the list.
  cut(cache->list_storage, &cache->nodes, index);
  set_head(cache->list_storage, &cache->nodes, index);

  // Here is the value of the element.
  *value = cache->list_storage[index].value;

  return true;
}

HM_PUBLIC_API
void HM_CDECL hm_cache_dump(hm_cache_t* cache, uint32_t* ips, size_t* ips_len) {
  assert(*ips_len >= cache->capacity);

  // Make sure that lists' emptiness is consistent in head_index and tail_index.
  assert((cache->nodes.head_index == NO_INDEX) ==
         (cache->nodes.tail_index == NO_INDEX));
  assert((cache->free_nodes.head_index == NO_INDEX) ==
         (cache->free_nodes.tail_index == NO_INDEX));

  // Make sure that not both lists are empty.
  assert((cache->nodes.head_index != NO_INDEX) ||
         (cache->free_nodes.head_index != NO_INDEX));

  // Make sure that prev_index and next_index are always consistent.
  for (uint32_t i = 0; i < cache->capacity; i++) {
    uint32_t prev_index = cache->list_storage[i].prev_index;
    uint32_t next_index = cache->list_storage[i].next_index;
    if (prev_index != NO_INDEX) {
      assert(cache->list_storage[prev_index].next_index == i);
    }
    if (next_index != NO_INDEX) {
      assert(cache->list_storage[next_index].prev_index == i);
    }
  }

  // Make sure the list and free list cover have correct total capacity.
  size_t list_size = 0, free_list_size = 0, list_size2 = 0, free_list_size2 = 0;
  for (uint32_t i = cache->nodes.head_index; i != NO_INDEX;
       i = cache->list_storage[i].next_index) {
    list_size++;
  }
  for (uint32_t i = cache->free_nodes.head_index; i != NO_INDEX;
       i = cache->list_storage[i].next_index) {
    free_list_size++;
  }
  for (uint32_t i = cache->nodes.tail_index; i != NO_INDEX;
       i = cache->list_storage[i].prev_index) {
    list_size2++;
  }
  for (uint32_t i = cache->free_nodes.tail_index; i != NO_INDEX;
       i = cache->list_storage[i].prev_index) {
    free_list_size2++;
  }
  assert(list_size == list_size2);
  assert(free_list_size == free_list_size2);
  assert(list_size + free_list_size == cache->capacity);

  // Make sure the hash table and the list are consistent.
  for (uint32_t i = cache->nodes.head_index; i != NO_INDEX;
       i = cache->list_storage[i].next_index) {
    uint32_t ip = cache->list_storage[i].ip;
    assert(table_lookup(cache, ip) == i);
  }

  // Make sure the caller provided enough memory to save the list.
  assert(list_size <= *ips_len);

  // Dump the list into the array.
  size_t used = 0;
  for (uint32_t i = cache->nodes.head_index; i != NO_INDEX;
       i = cache->list_storage[i].next_index) {
    uint32_t ip = cache->list_storage[i].ip;
    ips[used] = ip;
    used++;
  }

  *ips_len = used;
}
