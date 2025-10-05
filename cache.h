#ifndef HM_CACHE_H
#define HM_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct hm_cache;

// hm_database_t is in-memory cache type.
typedef struct hm_cache hm_cache_t;

// hm_cache_place_size calculates cache_place size (bytes).
// capacity must be a power of 2, at least 2.
// Result is written to cache_place_size.
// Argument capacity is the maximum number of IPs stored in the cache.
// Speed must be in the range 1-5. The higher the number, the more
// memory is used, but the faster it works.
hm_error_t HM_CDECL hm_cache_place_size(size_t* cache_place_size,
                                        unsigned int capacity, int speed);

// hm_cache_init creates a cache of IP addresses.
// Memory referenced by cache_place of size calculated by hm_cache_place_size
// is used to store the cache.
// Argument capacity is the maximum number of IPs stored in the cache.
// Speed must be in the range 1-5. The higher the number, the more
// memory is used, but the faster it works.
hm_error_t HM_CDECL hm_cache_init(char* cache_place, size_t cache_place_size,
                                  hm_cache_t** cache_ptr, unsigned int capacity,
                                  int speed);

// hm_cache_add (re)adds IP to the cache.
// Output "existed" is used to store if the IP has existed before.
// Output "evicted" is used to store if some IP was evicted.
// Output "evicted_ip" is used to store the evicted IP.
void HM_CDECL hm_cache_add(hm_cache_t* cache, const uint32_t ip,
                           const uint32_t value, bool* existed, bool* evicted,
                           uint32_t* evicted_ip, uint32_t* evicted_value);

// hm_cache_remove removes IP from the cache if it belongs to it.
void HM_CDECL hm_cache_remove(hm_cache_t* cache, const uint32_t ip,
                              bool* existed, uint32_t* existed_value);

// hm_cache_has returns if the cache has the IP.
// The position of the IP in the cache is updated, hence the first argument
// is not contant.
bool HM_CDECL hm_cache_has(hm_cache_t* cache, const uint32_t ip,
                           uint32_t* value);

// hm_cache_dump checks consistency of internal structures and dumps the linked
// list, from neweset to oldest. ips_len should point to the size of ips array,
// it must be greater or equal to capacity. It is modified by the function to
// the actual length of the dumped list.
void HM_CDECL hm_cache_dump(hm_cache_t* cache, uint32_t* ips, size_t* ips_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // HM_CACHE_H
