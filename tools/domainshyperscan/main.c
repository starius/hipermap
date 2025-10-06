#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "domain_map_db.h"

int main(void)
{
    printf("Loading domains.txt\n");
    const char* root_path = "."; // Adjust the path as needed
    const char* filename = "domains.txt";

    struct timespec start_tspec, db_load_tspec;
    clock_gettime(CLOCK_REALTIME, &start_tspec);
    // Load the domain map database
    DomainMapDB db = domain_map_db_load_from_file(root_path, filename);
    if (db == NULL)
    {
        fprintf(stderr, "Failed to load domain map database\n");
        return EXIT_FAILURE;
    }
    clock_gettime(CLOCK_REALTIME, &db_load_tspec);
    printf("Loaded %u domains\n", domain_map_db_get_count(db));

    // Create a scratch space for matching
    DomainMapDBScratch scratch = domain_map_db_scratch_create_empty();
    if (scratch == NULL)
    {
        fprintf(stderr, "Failed to create scratch space\n");
        domain_map_db_destroy(db);
        return EXIT_FAILURE;
    }
    int result = domain_map_db_scratch_adjust_to_db(scratch, db);
    if (result != EXIT_SUCCESS)
    {
        fprintf(stderr, "Failed to adjust scratch space\n");
        domain_map_db_destroy(db);
        domain_map_db_scratch_destroy(scratch);
        return EXIT_FAILURE;
    }
    struct timespec scratch_adjust_tspec;
    clock_gettime(CLOCK_REALTIME, &scratch_adjust_tspec);


    // Perform lookups
    const char* d1 = "apis.google.com";
    const char* d2 = "yandex.ru";
    const char* d3 = "microsoft.com";
    const char* d4 = "long.very-long.domain.com";

    struct timespec d1_match_tspec, d2_match_tspec, d3_match_tspec, d4_match_tspec;

    result = domain_map_db_match(db, scratch, d1);
    if (result == 1)
    {
        const uint32_t match_id = domain_map_db_scratch_get_match_id(scratch);
        printf("Match found for %s, index: %u, matched record: %s\n",
               d1, match_id, domain_map_db_get_record(db, match_id));
    }
    else
    {
        printf("No match for %s\n", d1);
    }
    clock_gettime(CLOCK_REALTIME, &d1_match_tspec);

    result = domain_map_db_match(db, scratch, d2);
    if (result == 1)
    {
        const uint32_t match_id = domain_map_db_scratch_get_match_id(scratch);
        printf("Match found for %s, index: %u, matched record: %s\n",
               d2, match_id, domain_map_db_get_record(db, match_id));
    }
    else
    {
        printf("No match for %s\n", d2);
    }
    clock_gettime(CLOCK_REALTIME, &d2_match_tspec);

    result = domain_map_db_match(db, scratch, d3);
    if (result == 1)
    {
        const uint32_t match_id = domain_map_db_scratch_get_match_id(scratch);
        printf("Match found for %s, index: %u, matched record: %s\n",
               d3, match_id, domain_map_db_get_record(db, match_id));
    }
    else
    {
        printf("No match for %s\n", d3);
    }
    clock_gettime(CLOCK_REALTIME, &d3_match_tspec);

    result = domain_map_db_match(db, scratch, d4);
    if (result == 1)
    {
        const uint32_t match_id = domain_map_db_scratch_get_match_id(scratch);
        printf("Match found for %s, index: %u, matched record: %s\n",
               d4, match_id, domain_map_db_get_record(db, match_id));
    }
    else
    {
        printf("No match for %s\n", d4);
    }
    clock_gettime(CLOCK_REALTIME, &d4_match_tspec);

    // results
    printf("Time results:\n");
    printf("Load time: %ld.%09ld seconds\n",
           db_load_tspec.tv_sec - start_tspec.tv_sec,
           db_load_tspec.tv_nsec - start_tspec.tv_nsec);
    printf("Scratch time: %ld.%09ld seconds\n",
           scratch_adjust_tspec.tv_sec - db_load_tspec.tv_sec,
           scratch_adjust_tspec.tv_nsec - db_load_tspec.tv_nsec);
    printf("D1 match time: %ld.%09ld seconds\n",
           d1_match_tspec.tv_sec - scratch_adjust_tspec.tv_sec,
           d1_match_tspec.tv_nsec - scratch_adjust_tspec.tv_nsec);
    printf("D2 match time: %ld.%09ld seconds\n",
           d2_match_tspec.tv_sec - d1_match_tspec.tv_sec,
           d2_match_tspec.tv_nsec - d1_match_tspec.tv_nsec);
    printf("D3 match time: %ld.%09ld seconds\n",
           d3_match_tspec.tv_sec - d2_match_tspec.tv_sec,
           d3_match_tspec.tv_nsec - d2_match_tspec.tv_nsec);
    printf("D4 match time: %ld.%09ld seconds\n",
           d4_match_tspec.tv_sec - d3_match_tspec.tv_sec,
           d4_match_tspec.tv_nsec - d3_match_tspec.tv_nsec);
    domain_map_db_destroy(db);
    domain_map_db_scratch_destroy(scratch);

    return EXIT_SUCCESS;
}
