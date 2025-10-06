// XXH3 one-shot hashing for spans, inlined via header-only mode.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL 1
#endif
#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY 1
#endif
#include "cvendor/xxhash.h"

// Hash of [start, start+length). No case folding is performed; callers are
// expected to normalize (e.g., lowercase) earlier in the pipeline when needed.
// 'seed' lets you chain multiple spans: pass previous hash as seed for the next
// span (XXH3 supports arbitrary 64-bit seed).
static inline uint64_t hash64_span_ci(const char* start, size_t length,
                                      uint64_t seed) {
  return (uint64_t)XXH3_64bits_withSeed((const void*)start, length,
                                        (XXH64_hash_t)seed);
}
