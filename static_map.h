#ifndef HM_STATIC_MAP_H
#define HM_STATIC_MAP_H

#include <stdint.h>

// hm_error_t is error type returned by some functions of hipermap.
typedef int hm_error_t;

// hm_success is returned in case of success.
const hm_error_t hm_success = 0;

// hm_error_bad_alignment is returned if db_place is not 8 byte aligned.
const hm_error_t hm_error_bad_alignment = 1;

// hm_error_small_place is returned if db_place is too small.
const hm_error_t hm_error_small_place = 2;

// hm_error_no_masks is returned if the number of masks is 0.
const hm_error_t hm_error_no_masks = 3;

// hm_no_value is a value meaning "no value".
const uint64_t hm_no_value = 0xFFFFFFFFFFFFFFFF;

// hm_database_t is in-memory database type.
typedef struct hm_database hm_database_t;

// hm_db_place_size returns db_place size.
size_t hm_db_place_size(unsigned int elements);

// hm_compile compiles the database of IP/prefix masks with corresponding
// values. db_place must be a memory buffer of size hm_db_place_size(elements).
// After a successfull call db_ptr points to a pointer to hm_database_t
// structure, which can be used in hm_find calls. The function allocates and
// deallocates dynamic memory during execution.
hm_error_t hm_compile(char *db_place, size_t db_place_size,
                      hm_database_t **db_ptr, const uint32_t *ips,
                      const uint8_t *cidr_prefixes, const uint64_t *values,
                      unsigned int elements);

// hm_place_used returns how many bytes of db_place are actually used.
// db_place can be cut to this size before writing to file.
size_t hm_place_used(const hm_database_t *db);

// hm_find returns the value corresponding to the given IP in the database.
uint64_t hm_find(const hm_database_t *db, const uint32_t ip);

// hm_db_from_place recovers database from db_place bytes.
// It can be stored and loaded in machine with the same endianess.
// After a successfull call db_ptr points to a pointer to hm_database_t
// structure, which can be used in hm_find calls. db_place can be modified
// during the call.
hm_error_t hm_db_from_place(char *db_place, size_t db_place_size,
                            hm_database_t **db);

#endif // HM_STATIC_MAP_H
