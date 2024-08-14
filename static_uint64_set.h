#ifndef HM_STATIC_UINT64_SET_H
#define HM_STATIC_UINT64_SET_H

#include <stdbool.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct hm_u64_database;

// hm_u64_database_t is in-memory database type for static set of uint64.
typedef struct hm_u64_database hm_u64_database_t;

// hm_u64_db_place_size returns db_place size for static set of uint64.
size_t HM_CDECL hm_u64_db_place_size(unsigned int elements);

// hm_u64_compile compiles the database of uint64 keys. db_place must be a
// memory buffer of size hm_u64_db_place_size(elements). After a successfull
// call db_ptr points to a pointer to hm_u64_database_t structure, which can be
// used in hm_u64_find calls. Keys must be unique and 0 is not allowed as key,
// otherwise HM_ERROR_BAD_VALUE is returned.
hm_error_t HM_CDECL hm_u64_compile(char *db_place, size_t db_place_size,
                                   hm_u64_database_t **db_ptr,
                                   const uint64_t *keys, unsigned int elements);

// hm_u64_find returns if the given uint64 key is present in the database.
bool HM_CDECL hm_u64_find(const hm_u64_database_t *db, const uint64_t key);

// hm_u64_serialized_size returns how many bytes are needed to serialize the db.
size_t HM_CDECL hm_u64_serialized_size(const hm_u64_database_t *db);

// hm_u64_serialize serializes db to buffer.
// Buffer size must be the equal to the one returned by hm_u64_serialized_size.
// It can be stored and loaded in machine with the same endianess.
hm_error_t HM_CDECL hm_u64_serialize(char *buffer, size_t buffer_size,
                                     const hm_u64_database_t *db);

// hm_u64_db_place_size_from_serialized returns size needed for db_place
// using the buffer with serialized db as an input.
hm_error_t HM_CDECL hm_u64_db_place_size_from_serialized(size_t *db_place_size,
                                                         const char *buffer,
                                                         size_t buffer_size);

// hm_u64_deserialize deserializes db from buffer.
// db_place_size must be the equal to the one returned by
// hm_u64_db_place_size_from_serialized. After a successfull call db_ptr points
// to a pointer to hm_u64_database_t structure, which can be used in hm_u64_find
// calls. db_place can be modified during the call.
hm_error_t HM_CDECL hm_u64_deserialize(char *db_place, size_t db_place_size,
                                       hm_u64_database_t **db_ptr,
                                       const char *buffer, size_t buffer_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // HM_STATIC_MAP_H
