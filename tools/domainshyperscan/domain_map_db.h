#ifndef DOMAIN_MAP_DB_H
#define DOMAIN_MAP_DB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct domain_map_db_t* DomainMapDB;
typedef struct domain_map_db_scratch_t* DomainMapDBScratch;

DomainMapDBScratch
domain_map_db_scratch_create_empty();

uint32_t
domain_map_db_scratch_get_match_id(
    DomainMapDBScratch scratch);


DomainMapDB
domain_map_db_load_from_file(
    const char* root_path,
    const char* filename);

// Build a database from an in-memory list of domain patterns.
DomainMapDB
domain_map_db_build_from_patterns(
    const char* const* patterns,
    uint32_t count);

char*
domain_map_db_get_name(
    DomainMapDB db);

uint32_t
domain_map_db_get_count(
    DomainMapDB db);

void
domain_map_db_destroy(
    DomainMapDB db);


DomainMapDB
domain_map_db_clone(DomainMapDB source);

int
domain_map_db_scratch_adjust_to_db(
    DomainMapDBScratch scratch,
    DomainMapDB db);

void
domain_map_db_scratch_destroy(DomainMapDBScratch scratch);

/**
 * Find the domain in the list
 * @param db
 * @param scratch
 * @param domain
 * @return 1 if found, 0 if not found, -1 on error
 */
int
domain_map_db_match(
    DomainMapDB db,
    DomainMapDBScratch scratch,
    const char* domain);

char*
domain_map_db_get_record(
    DomainMapDB db,
    uint32_t match_id);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //DOMAIN_MAP_DB_H
