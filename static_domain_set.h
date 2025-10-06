#ifndef HM_STATIC_DOMAIN_SET_H
#define HM_STATIC_DOMAIN_SET_H

#include <stdbool.h>
#include <stddef.h>  // size_t
#include <stdint.h>

#include "common.h"

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "static_domain_set assumes little-endian targets"
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct hm_domain_database;

// hm_domain_database_t is in-memory database type for static set of domains.
typedef struct hm_domain_database hm_domain_database_t;

// hm_domain_db_place_size returns an upper-bound db_place size for the given
// input domains. The size intentionally over-allocates to accommodate seed and
// bucket calibration and alignment/tail safety. To minimize memory, users are
// encouraged to serialize the built DB and then deserialize it into a
// right-sized buffer. In case of errors returns 0.
size_t HM_CDECL hm_domain_db_place_size(const char** domains,
                                        unsigned int elements);

// hm_domain_compile compiles the database of domains. db_place must be a
// memory buffer of size hm_domain_db_place_size(domains, elements). After a
// successfull call db_ptr points to a pointer to hm_domain_database_t
// structure, which can be used in hm_domain_find calls. Keys must be unique and
// "" is not allowed as key, otherwise HM_ERROR_BAD_VALUE is returned.
// This type uses popular domains optimizations and has an upper limit on the
// number of popular domain suffixes: 256. Otherwise it returns error
// HM_ERROR_TOO_MANY_POPULAR_DOMAINS.
hm_error_t HM_CDECL hm_domain_compile(char* db_place, size_t db_place_size,
                                      hm_domain_database_t** db_ptr,
                                      const char** domains,
                                      unsigned int elements);

// hm_domain_find returns if the given domains key is present in the database.
// Results: 0 - not found, 1 - found, (-1) - invalid input.
int HM_CDECL hm_domain_find(const hm_domain_database_t* db, const char* domain,
                            size_t domain_len);

// hm_domain_serialized_size returns how many bytes are needed to serialize the
// db.
size_t HM_CDECL hm_domain_serialized_size(const hm_domain_database_t* db);

// hm_domain_serialize serializes db to buffer.
// Buffer size must be the equal to the one returned by
// hm_domain_serialized_size. It can be stored and loaded in machine with the
// same endianess.
hm_error_t HM_CDECL hm_domain_serialize(char* buffer, size_t buffer_size,
                                        const hm_domain_database_t* db);

// hm_domain_db_place_size_from_serialized returns size needed for db_place
// using the buffer with serialized db as an input.
hm_error_t HM_CDECL hm_domain_db_place_size_from_serialized(
    size_t* db_place_size, const char* buffer, size_t buffer_size);

// hm_domain_deserialize deserializes db from buffer.
// db_place_size must be the equal to the one returned by
// hm_domain_db_place_size_from_serialized. After a successfull call db_ptr
// points to a pointer to hm_domain_database_t structure, which can be used in
// hm_domain_find calls. db_place can be modified during the call.
hm_error_t HM_CDECL hm_domain_deserialize(char* db_place, size_t db_place_size,
                                          hm_domain_database_t** db_ptr,
                                          const char* buffer,
                                          size_t buffer_size);

//////////////////////////////////////////////////////////////////
// Functions below are used in Go bindings and test suite only. //
//////////////////////////////////////////////////////////////////

// hm_domain_hash returns the hash used internally for a buffer with the
// provided seed. Returns lower 16 bits. Callers should normalize case earlier
// if needed. The hash used is xxhash3.
uint16_t HM_CDECL hm_domain_hash(const char* s, size_t len, uint64_t seed);

// hm_domain_hash64_span_ci exposes the 64-bit hash over a span with a 64-bit
// seed for stronger test coverage. No case folding is performed.
uint64_t HM_CDECL hm_domain_hash64_span_ci(const char* s, size_t len,
                                           uint64_t seed);

// hm_cut_last_domain_label_offset returns the byte offset into `domain`
// where the last label starts. If there is no dot or the input is empty,
// returns 0. Trailing dots must be removed beforehand.
size_t HM_CDECL hm_cut_last_domain_label_offset(const char* domain, size_t len);

// Lightweight getters for summary information used by Go bindings.
uint32_t HM_CDECL hm_domain_buckets(const hm_domain_database_t* db);
uint32_t HM_CDECL hm_domain_popular_count(const hm_domain_database_t* db);
uint32_t HM_CDECL hm_domain_used_total(const hm_domain_database_t* db);
uint32_t HM_CDECL hm_domain_hash_seed(const hm_domain_database_t* db);
size_t HM_CDECL hm_domain_header_bytes();
size_t HM_CDECL hm_domain_table_bytes(const hm_domain_database_t* db);
size_t HM_CDECL hm_domain_popular_bytes(const hm_domain_database_t* db);
size_t HM_CDECL hm_domain_blob_bytes(const hm_domain_database_t* db);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // HM_STATIC_DOMAIN_SET_H
