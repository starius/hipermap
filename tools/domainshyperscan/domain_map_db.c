
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/limits.h>

#include <hs.h>

#include "linked_str_list.h"
#include "domain_map_db.h"

#define MAX_LINE_LENGTH 1024

static const char* special_chars = ".^$*+?()[]{}|\\";

struct domain_map_db_t
{
    char* name;
    size_t nb_records;
    char** domains;
    char** domains_regex;
    hs_database_t* db;
    uint8_t is_clone;
};

struct domain_map_db_scratch_t
{
    hs_scratch_t* scratch;
    uint32_t match_id;
    uint8_t has_match;
};


static
char*
escape_regex_chars(char* str)
{
    if (str == NULL)
    {
        return NULL;
    }
    char* escaped_str = NULL;

    size_t final_length = 0;
    char* c = str;
    while (*c)
    {
        if (strchr(special_chars, *c) != NULL)
        {
            final_length += 1;
        }
        final_length += 1;
        c++;
    }
    if (final_length > MAX_LINE_LENGTH)
    {
        return NULL;
    }
    // will start with . and be escaped
    final_length = final_length + 2;
    // ending $ character
    escaped_str = malloc(final_length + 2);
    if (escaped_str == NULL)
    {
        return NULL;
    }

    c = str;
    escaped_str[0] = '\\';
    escaped_str[1] = '.';
    char* o = escaped_str + 2;
    while (*c)
    {
        if (strchr(special_chars, *c) != NULL)
        {
            *o++ = '\\';
        }
        *o++ = *c++;
    }
    *o = '$';
    o++;
    *o = '\0';
    return escaped_str;
}

static int hs_match_event_handler(unsigned int id,
                                  unsigned long long from,
                                  unsigned long long len,
                                  unsigned int flags,
                                  void* context)
{
    (void)from;
    (void)len;
    (void)flags;
    struct domain_map_db_scratch_t* ctx = (struct domain_map_db_scratch_t*)context;
    if (ctx == NULL)
    {
        return 0;
    }
    ctx->match_id = id;
    ctx->has_match = 1;
    return 1;
}

static struct domain_map_db_t* domain_map_db_create(const char* name)
{
    struct domain_map_db_t* db = malloc(sizeof(struct domain_map_db_t));
    if (db == NULL)
    {
        return NULL;
    }
    memset(db, 0, sizeof(struct domain_map_db_t));
    db->name = strdup(name);
    if (db->name == NULL)
    {
        domain_map_db_destroy(db);
        return NULL;
    }
    return db;
}


char* trim_white_space(char* line)
{
    size_t len = strlen(line);
    size_t iter = 0;

    while (isspace(line[0]) && iter < len)
    {
        iter++;
        line++;
    }
    if (line[0] == '\0' || iter == len)
    {
        return NULL;
    }
    len = len - iter;
    int64_t i;
    for (i = (int64_t)len - 1; i >= 0 && isspace(line[i]); i--)
    {
        // DO NOTHING
    }
    line[i + 1] = '\0';
    return line;
}


void
char_array_destroy(const size_t nb, char** array)
{
    for (size_t i = 0; i < nb; i++)
    {
        if (array[i] != NULL)
        {
            free(array[i]);
        }
        array[i] = NULL;
    }
    free(array);
}


DomainMapDB
domain_map_db_load_from_file(
    const char* root_path,
    const char* filename)
{
    char file_path[PATH_MAX];
    const int ret = snprintf(file_path, PATH_MAX, "%s/%s", root_path, filename);
    if (ret < 0 || ret >= PATH_MAX)
    {
        return NULL;
    }
    struct domain_map_db_t* db = domain_map_db_create(filename);
    if (db == NULL)
    {
        return NULL;
    }
    FILE* file = fopen(file_path, "r");
    if (file == NULL)
    {
        domain_map_db_destroy(db);
        return NULL;
    }

    char line[MAX_LINE_LENGTH];

    LinkedStrList domain_list = linked_str_list_create();
    if (domain_list == NULL)
    {
        fclose(file);
        domain_map_db_destroy(db);
        return NULL;
    }

    while (fgets(line, MAX_LINE_LENGTH, file) != NULL)
    {
        const char* trimmed_line = trim_white_space(line);
        if (trimmed_line == NULL)
        {
            continue;
        }
        if (strncmp(trimmed_line, "#", 1) == 0)
        {
            continue;
        }
        char* value = strdup(trimmed_line);
        if (value == NULL)
        {
            continue;
        }
        linked_str_list_add_value(domain_list, value);
    }
    fclose(file);

    const uint32_t count = linked_str_list_count(domain_list);
    if (count == 0)
    {
        linked_str_list_destroy(domain_list);
        db->domains_regex = NULL;
        db->domains = NULL;
        db->db = NULL;
    }
    else
    {
        char** domains = linked_str_list_to_array(domain_list);
        if (domains == NULL)
        {
            linked_str_list_destroy(domain_list);
            domain_map_db_destroy(db);
            return NULL;
        }
        linked_str_list_destroy(domain_list);
        char** domains_regex = malloc(sizeof(char*) * count);
        if (domains_regex == NULL)
        {
            domain_map_db_destroy(db);
            return NULL;
        }
        unsigned int* ids = malloc(sizeof(unsigned int) * count);
        if (ids == NULL)
        {
            domain_map_db_destroy(db);
            free(domains_regex);
            return NULL;
        }
        unsigned* flags = malloc(sizeof(unsigned) * count);
        if (flags == NULL)
        {
            domain_map_db_destroy(db);
            free(domains_regex);
            free(ids);
            return NULL;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            ids[i] = (unsigned int)i;
            flags[i] = HS_FLAG_CASELESS;
            domains_regex[i] = escape_regex_chars(domains[i]);
        }
        hs_compile_error_t* compile_error = NULL;
        hs_database_t* hs_db = NULL;
        const hs_error_t hs_err = hs_compile_multi(
            (const char*const*)domains_regex,
            flags,
            ids,
            count,
            HS_MODE_BLOCK,
            NULL,
            &hs_db,
            &compile_error);
        if (hs_err != HS_SUCCESS)
        {
            if (compile_error != NULL)
            {
                // error compiling domain database, compile_error->message
                hs_free_compile_error(compile_error);
            }
            else
            {
                // error compiling domain database
            }
            domain_map_db_destroy(db);
            char_array_destroy(count, domains);
            char_array_destroy(count, domains_regex);
            free(ids);
            free(flags);
            if (hs_db != NULL)
            {
                hs_free_database(hs_db);
            }
            return NULL;
        }
        free(ids);
        free(flags);
        db->db = hs_db;
        db->domains = domains;
        db->domains_regex = domains_regex;
    }
    db->nb_records = count;
    return db;
}


char*
domain_map_db_get_name(
    DomainMapDB db)
{
    return db->name;
}

uint32_t
domain_map_db_get_count(
    DomainMapDB db)
{
    return db->nb_records;
}

void domain_map_db_destroy(DomainMapDB db)
{
    if (db == NULL)
    {
        return;
    }
    if (db->db != NULL)
    {
        if (db->is_clone == 0)
        {
            hs_free_database(db->db);
        }
        else
        {
            free(db->db);
        }
        db->db = NULL;
    }
    if (db->name != NULL)
    {
        free(db->name);
        db->name = NULL;
    }
    if (db->domains != NULL)
    {
        char_array_destroy(db->nb_records, db->domains);
        db->domains = NULL;
    }
    if (db->domains_regex != NULL)
    {
        char_array_destroy(db->nb_records, db->domains_regex);
        db->domains_regex = NULL;
    }
    free(db);
}


DomainMapDB domain_map_db_clone(DomainMapDB source)
{
    struct domain_map_db_t* clone = domain_map_db_create(source->name);
    if (clone == NULL)
    {
        return NULL;
    }
    clone->nb_records = source->nb_records;
    if (clone->nb_records == 0)
    {
        clone->domains_regex = NULL;
        clone->domains = NULL;
        clone->db = NULL;
        return clone;
    }
    const size_t domains_size = sizeof(char*) * clone->nb_records;
    const size_t domains_regex_size = sizeof(char*) * clone->nb_records;
    clone->domains = malloc(domains_size);
    if (clone->domains == NULL)
    {
        domain_map_db_destroy(clone);
        return NULL;
    }
    memset(clone->domains, 0, domains_size);
    clone->domains_regex = malloc(domains_regex_size);
    if (clone->domains_regex == NULL)
    {
        domain_map_db_destroy(clone);
        return NULL;
    }
    memset(clone->domains_regex, 0, domains_regex_size);
    for (size_t i = 0; i < source->nb_records; i++)
    {
        clone->domains[i] = strdup(source->domains[i]);
        if (clone->domains[i] == NULL)
        {
            domain_map_db_destroy(clone);
            return NULL;
        }
        clone->domains_regex[i] = strdup(source->domains_regex[i]);
        if (clone->domains_regex[i] == NULL)
        {
            domain_map_db_destroy(clone);
            return NULL;
        }
    }
    clone->is_clone = 1;

    if (source->db == NULL)
    {
        clone->db = NULL;
        return clone;
    }

    char* buffer = NULL;
    size_t db_size = 0;
    hs_error_t hs_err = hs_serialize_database(source->db, &buffer, &db_size);
    if (hs_err != HS_SUCCESS)
    {
        domain_map_db_destroy(clone);
        return NULL;
    }
    hs_database_t* hs_db = malloc(db_size);
    if (hs_db == NULL)
    {
        domain_map_db_destroy(clone);
        free(buffer);
        return NULL;
    }
    hs_err = hs_deserialize_database_at(buffer, db_size, hs_db);
    if (hs_err != HS_SUCCESS)
    {
        domain_map_db_destroy(clone);
        free(buffer);
        free(hs_db);
        return NULL;
    }
    clone->db = hs_db;
    free(buffer);
    return clone;
}

DomainMapDBScratch domain_map_db_scratch_create_empty()
{
    size_t scratch_size = sizeof(struct domain_map_db_scratch_t);
    struct domain_map_db_scratch_t* scratch = malloc(scratch_size);
    if (scratch == NULL)
    {
        return NULL;
    }
    memset(scratch, 0, sizeof(struct domain_map_db_scratch_t));
    return scratch;
}

int
domain_map_db_scratch_adjust_to_db(
    DomainMapDBScratch scratch,
    DomainMapDB db)
{
    if (db->db == NULL)
    {
        return EXIT_SUCCESS;
    }
    hs_error_t hs_err = hs_alloc_scratch(db->db, &scratch->scratch);
    if (hs_err != HS_SUCCESS)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void
domain_map_db_scratch_destroy(DomainMapDBScratch scratch)
{
    if (scratch == NULL)
    {
        return;
    }
    if (scratch->scratch != NULL)
    {
        hs_free_scratch(scratch->scratch);
        scratch->scratch = NULL;
    }
    free(scratch);
}

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
    const char* domain)
{
    if (db->db == NULL)
    {
        // no database, no matches
        return 0;
    }
    char lookup_buffer[MAX_LINE_LENGTH];
    lookup_buffer[0] = '.';
    strncpy(lookup_buffer + 1, domain, MAX_LINE_LENGTH - 1);
    const size_t lookup_length = strlen(lookup_buffer);
    scratch->has_match = 0;
    scratch->match_id = 0;

    const hs_error_t hs_err = hs_scan(
        db->db,
        lookup_buffer,
        lookup_length,
        0,
        scratch->scratch,
        hs_match_event_handler,
        scratch);
    if (hs_err != HS_SUCCESS && hs_err != HS_SCAN_TERMINATED)
    {
        return -EXIT_FAILURE;
    }
    if (scratch->has_match == 0)
    {
        return 0;
    }
    return 1;
}


uint32_t
domain_map_db_scratch_get_match_id(
    DomainMapDBScratch scratch)
{
    return scratch->match_id;
}

char*
domain_map_db_get_record(
    DomainMapDB db,
    uint32_t match_id)
{
    return db->domains[match_id];
}


DomainMapDB domain_map_db_build_from_patterns(const char* const* patterns,
                                              uint32_t count)
{
    struct domain_map_db_t* db = domain_map_db_create("in-memory");
    if (db == NULL) {
        return NULL;
    }
    if (count == 0) {
        db->domains_regex = NULL;
        db->domains = NULL;
        db->db = NULL;
        db->nb_records = 0;
        return db;
    }
    char** domains = malloc(sizeof(char*) * count);
    if (domains == NULL) {
        domain_map_db_destroy(db);
        return NULL;
    }
    memset(domains, 0, sizeof(char*) * count);
    for (uint32_t i = 0; i < count; i++) {
        domains[i] = strdup(patterns[i]);
        if (domains[i] == NULL) {
            char_array_destroy(i, domains);
            domain_map_db_destroy(db);
            return NULL;
        }
    }

    char** domains_regex = malloc(sizeof(char*) * count);
    if (domains_regex == NULL) {
        char_array_destroy(count, domains);
        domain_map_db_destroy(db);
        return NULL;
    }
    unsigned int* ids = malloc(sizeof(unsigned int) * count);
    if (ids == NULL) {
        free(domains_regex);
        char_array_destroy(count, domains);
        domain_map_db_destroy(db);
        return NULL;
    }
    unsigned* flags = malloc(sizeof(unsigned) * count);
    if (flags == NULL) {
        free(ids);
        free(domains_regex);
        char_array_destroy(count, domains);
        domain_map_db_destroy(db);
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        ids[i] = (unsigned int)i;
        flags[i] = HS_FLAG_CASELESS;
        domains_regex[i] = escape_regex_chars(domains[i]);
    }
    hs_compile_error_t* compile_error = NULL;
    hs_database_t* hs_db = NULL;
    const hs_error_t hs_err = hs_compile_multi(
        (const char* const*)domains_regex, flags, ids, count, HS_MODE_BLOCK,
        NULL, &hs_db, &compile_error);
    if (hs_err != HS_SUCCESS) {
        if (compile_error != NULL) {
            hs_free_compile_error(compile_error);
        }
        domain_map_db_destroy(db);
        char_array_destroy(count, domains);
        char_array_destroy(count, domains_regex);
        free(ids);
        free(flags);
        if (hs_db != NULL) {
            hs_free_database(hs_db);
        }
        return NULL;
    }
    free(ids);
    free(flags);
    db->db = hs_db;
    db->domains = domains;
    db->domains_regex = domains_regex;
    db->nb_records = count;
    return db;
}
