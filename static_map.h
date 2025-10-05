#ifndef HM_STATIC_MAP_H
#define HM_STATIC_MAP_H

#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// HM_NO_VALUE is a value meaning "no value".
#define HM_NO_VALUE ((uint64_t)(0xFFFFFFFFFFFFFFFF))

struct hm_sm_database;

// hm_sm_database_t is in-memory database type.
typedef struct hm_sm_database hm_sm_database_t;

// hm_sm_db_place_size returns db_place size.
size_t HM_CDECL hm_sm_db_place_size(unsigned int elements);

// hm_sm_compile compiles the database of IP/prefix masks with corresponding
// values. db_place must be a memory buffer of size
// hm_sm_db_place_size(elements). After a successfull call db_ptr points to a
// pointer to hm_sm_database_t structure, which can be used in hm_sm_find calls.
// The function allocates and deallocates dynamic memory during execution.
hm_error_t HM_CDECL hm_sm_compile(char* db_place, size_t db_place_size,
                                  hm_sm_database_t** db_ptr,
                                  const uint32_t* ips,
                                  const uint8_t* cidr_prefixes,
                                  const uint64_t* values,
                                  unsigned int elements);

// hm_sm_find returns the value corresponding to the given IP in the database.
uint64_t HM_CDECL hm_sm_find(const hm_sm_database_t* db, const uint32_t ip);

// hm_sm_serialized_size returns how many bytes are needed to serialize db.
size_t HM_CDECL hm_sm_serialized_size(const hm_sm_database_t* db);

// hm_sm_serialize serializes db to buffer.
// Buffer size must be the equal to the one returned by hm_sm_serialized_size.
// It can be stored and loaded in machine with the same endianess.
hm_error_t HM_CDECL hm_sm_serialize(char* buffer, size_t buffer_size,
                                    const hm_sm_database_t* db);

// hm_sm_db_place_size_from_serialized returns size needed for db_place
// using the buffer with serialized db as an input.
hm_error_t HM_CDECL hm_sm_db_place_size_from_serialized(size_t* db_place_size,
                                                        const char* buffer,
                                                        size_t buffer_size);

// hm_sm_deserialize deserializes db from buffer.
// db_place_size must be the equal to the one returned by
// hm_sm_db_place_size_from_serialized. After a successfull call db_ptr points
// to a pointer to hm_sm_database_t structure, which can be used in hm_sm_find
// calls. db_place can be modified during the call.
hm_error_t HM_CDECL hm_sm_deserialize(char* db_place, size_t db_place_size,
                                      hm_sm_database_t** db_ptr,
                                      const char* buffer, size_t buffer_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // HM_STATIC_MAP_H
