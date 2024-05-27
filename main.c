#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <hs/hs.h>
#include <ipset/ipset.h>

#include "static_map.h"

const size_t IP_REGEXP_BUF_LEN =
    1 + 4 * 3 + 11 + 1; // ^, 3 bytes, one [] range, one zero byte.

size_t last_ip_byte_range(char *dst, uint8_t last_ip_byte,
                          uint8_t mask_bits_mod8) {
  uint8_t bitmask = 0xFF00 >> mask_bits_mod8;
  uint8_t first = last_ip_byte & bitmask;
  uint8_t last = first | (uint8_t)(0xFF >> mask_bits_mod8);
  return sprintf(dst, "[\\x%X-\\x%X]", first, last);
}

size_t ip_to_regexp(char *dst, int ip_bytes[4], int mask_bits) {
  char *cursor = dst;

  cursor[0] = '^';
  cursor++;

  for (int i = 0; i < mask_bits / 8; i++) {
    cursor += sprintf(cursor, "\\x%X", ip_bytes[i]);
  }

  if (mask_bits % 8 != 0) {
    cursor +=
        last_ip_byte_range(cursor, ip_bytes[mask_bits / 8], mask_bits % 8);
  }

  return cursor - dst;
}

// Fast hash function on uint32_t.
// See
// https://www.reddit.com/r/C_Programming/comments/vv8yql/weird_problem_print_every_32_bit_number_once_in/?rdt=35253
// See https://github.com/skeeto/hash-prospector/issues/19
uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x21f0aaad;
  x ^= x >> 15;
  x *= 0xd35a2d97;
  x ^= x >> 15;
  return x;
}

struct match_context {
  int count;    // Set to 0 when initializing.
  int final_id; // Set to -1 when initializing.
  int prev_id;  // Set to -1 when initializing.
};

int matcher(unsigned int id, unsigned long long from, unsigned long long to,
            unsigned int flags, void *context) {
  struct match_context *c = context;
  c->count++;
  if (c->final_id != -1) {
    c->prev_id = c->final_id;
  }
  c->final_id = id;
  return 0;
}

void fill_ip(char *bytes, uint32_t ip) {
  bytes[0] = (ip >> 24) & 0xFF;
  bytes[1] = (ip >> 16) & 0xFF;
  bytes[2] = (ip >> 8) & 0xFF;
  bytes[3] = ip & 0xFF;
}

const size_t alignment = 8;

char *align8(char *addr) {
  return (char *)(((uintptr_t)(addr) & ~(alignment - 1)) + alignment);
}

void print_ip(uint32_t ip) {
  unsigned char bytes[4];
  fill_ip(bytes, ip);
  printf("%d.%d.%d.%d\n", bytes[0], bytes[1], bytes[2], bytes[3]);
}

int main(int argc, char *argv[]) {
  const char *masks_file = argv[1];
  FILE *fp = fopen(masks_file, "r");
  if (fp == NULL) {
    printf("Error: could not open file %s.\n", masks_file);
    return 1;
  }

  char **patterns = NULL;
  uint32_t *ips = NULL;
  uint8_t *cidr_prefixes = NULL;
  uint64_t *values = NULL;
  size_t len = 0;
  size_t cap = 0;

  ipset_init_library();

  struct ip_set *ip_set = ipset_new();

  while (1) {
    int ip_bytes[4];
    int mask_bits;
    int r = fscanf(fp, "%d.%d.%d.%d/%d", &ip_bytes[0], &ip_bytes[1],
                   &ip_bytes[2], &ip_bytes[3], &mask_bits);
    if (r == EOF) {
      break;
    }
    if (r == 5) {
      // Match.
      char *pattern = malloc(IP_REGEXP_BUF_LEN); // FIXME memory leak
      size_t pattern_size = ip_to_regexp(pattern, ip_bytes, mask_bits);
      assert(pattern_size <= IP_REGEXP_BUF_LEN);
      if (len == cap) {
        cap = (cap + 1) * 2;
        patterns = realloc(patterns, cap * sizeof(char *));
        ips = realloc(ips, cap * sizeof(uint32_t));
        cidr_prefixes = realloc(cidr_prefixes, cap * sizeof(uint8_t));
        values = realloc(values, cap * sizeof(uint64_t));
      }
      patterns[len] = pattern;

      struct cork_ipv4 ip = {
          ._ = {.u8 = {ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]}}};
      ipset_ipv4_add_network(ip_set, &ip, mask_bits);

      ips[len] = ((uint32_t)(ip_bytes[0]) << 24) |
                 ((uint32_t)(ip_bytes[1]) << 16) |
                 ((uint32_t)(ip_bytes[2]) << 8) | (uint32_t)(ip_bytes[3]);
      cidr_prefixes[len] = mask_bits;
      values[len] = len;

      printf("IP: %d.%d.%d.%d/%d -> %s\n", ip_bytes[0], ip_bytes[1],
             ip_bytes[2], ip_bytes[3], mask_bits, pattern);

      len++;
    }

    // Read the rest of the line, a comment.
    const unsigned MAX_LENGTH = 256;
    char line[MAX_LENGTH];
    fgets(line, MAX_LENGTH, fp);
  }

  fclose(fp);

  unsigned int *ids = malloc(sizeof(int) * len);
  for (int i = 0; i < len; i++) {
    ids[i] = i;
  }

  const unsigned int *compile_flags = NULL;
  const hs_platform_info_t *platform = NULL;
  hs_database_t *db;
  hs_compile_error_t *hs_compile_err;
  hs_error_t hs_err;
  hs_err = hs_compile_multi((const char *const *)(patterns), compile_flags, ids,
                            len, HS_MODE_BLOCK, platform, &db, &hs_compile_err);
  if (hs_err == HS_COMPILER_ERROR) {
    int expr = hs_compile_err->expression;
    printf("hs_compile_multi failed for IP N %d, expression %s: %s.\n", expr,
           patterns[expr], hs_compile_err->message);
    hs_free_compile_error(hs_compile_err);
    return 1;
  }

  size_t db_size;
  hs_err = hs_database_size(db, &db_size);
  if (hs_err != HS_SUCCESS) {
    printf("hs_database_size failed: %d.\n", hs_err);
    return 1;
  }

  printf("size of hyperscan db is: %d bytes.\n", (int)(db_size));

  hs_scratch_t *scratch = NULL;
  hs_err = hs_alloc_scratch(db, &scratch);
  if (hs_err != HS_SUCCESS) {
    printf("hs_alloc_scratch failed: %d.\n", hs_err);
    return 1;
  }

  size_t hm_db_place_len = hm_db_place_size(len);
  char *hm_db_place0 = malloc(hm_db_place_len + alignment);
  char *hm_db_place = align8(hm_db_place0);
  hm_database_t *hm_db;
  hm_error_t hm_err = hm_compile(hm_db_place, hm_db_place_len, &hm_db, ips,
                                 cidr_prefixes, values, len);
  if (hm_err != HM_SUCCESS) {
    printf("hm_compile failed: %d.\n", hm_err);
    return 1;
  }
  size_t hm_used = hm_place_used(hm_db);
  printf("size of hipermap db is: %d/%d bytes.\n", (int)hm_used,
         (int)hm_db_place_len);

  // Check edge cases.
  uint64_t v1 = hm_find(hm_db, 0x00000000);
  uint64_t v2 = hm_find(hm_db, 0xFFFFFFFF);
  assert(v1 == HM_NO_VALUE);
  assert(v2 == HM_NO_VALUE);

  uint32_t ip = 1;
  const int SAMPLE_SIZE = 100000000;
  unsigned int length = 4;
  unsigned int scan_flags = 0;
  match_event_handler onEvent = matcher;

  // Compare results.
  unsigned char ip_bytes[4];
  for (int i = 0; i < SAMPLE_SIZE; i++) {
    ip = hash32(ip);
    fill_ip(ip_bytes, ip);
    struct match_context context = {
        .count = 0,
        .final_id = -1,
        .prev_id = -1,
    };
    hs_err =
        hs_scan(db, ip_bytes, length, scan_flags, scratch, onEvent, &context);
    if (hs_err != HS_SUCCESS) {
      printf("hs_scan failed: %d.\n", hs_err);
      return 1;
    }
    bool hyperscan_match = (context.count != 0);

    struct cork_ipv4 cip = {
        ._ = {.u8 = {ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]}}};
    bool ip_set_match = ipset_contains_ipv4(ip_set, &cip);

    uint64_t hm_res = hm_find(hm_db, ip);
    bool hm_match = hm_res != HM_NO_VALUE;

    if (hyperscan_match != ip_set_match) {
      printf("MISMATCH! hyperscan=%b ip_set=%b ", hyperscan_match,
             ip_set_match);
      print_ip(ip);
    }
    if (hyperscan_match != hm_match) {
      printf("MISMATCH! hyperscan_match=%d hm_match=%d hyperscan=%d hm=%" PRIu64
             " ",
             hyperscan_match, hm_match, context.final_id, hm_res);
      print_ip(ip);
    }
  }

  struct timespec benchmark_start, benchmark_stop;
  double benchmark_time;

  clock_gettime(CLOCK_MONOTONIC, &benchmark_start);
  int hyperscan_sum = 0;
  ip = 1;
  for (int i = 0; i < SAMPLE_SIZE; i++) {
    ip = hash32(ip);
    fill_ip(ip_bytes, ip);
    struct match_context context = {
        .count = 0,
        .final_id = -1,
        .prev_id = -1,
    };
    hs_err =
        hs_scan(db, ip_bytes, length, scan_flags, scratch, onEvent, &context);
    if (hs_err != HS_SUCCESS) {
      printf("hs_scan failed: %d.\n", hs_err);
      return 1;
    }
    if (context.count != 0) {
      hyperscan_sum++;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &benchmark_stop);
  benchmark_time =
      ((double)benchmark_stop.tv_sec + 1.0e-9 * benchmark_stop.tv_nsec) -
      ((double)benchmark_start.tv_sec + 1.0e-9 * benchmark_start.tv_nsec);
  printf("HYPERSCAN: %d IPs were analyzed in %f s.\n", SAMPLE_SIZE,
         benchmark_time);
  printf("One IP is analyzed in %g s\n", benchmark_time / SAMPLE_SIZE);
  printf("Found %d matches.\n", hyperscan_sum);

  clock_gettime(CLOCK_MONOTONIC, &benchmark_start);
  ip = 1;
  int ip_set_sum = 0;
  for (int i = 0; i < SAMPLE_SIZE; i++) {
    ip = hash32(ip);
    fill_ip(ip_bytes, ip);
    struct cork_ipv4 cip = {
        ._ = {.u8 = {ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]}}};
    if (ipset_contains_ipv4(ip_set, &cip)) {
      ip_set_sum++;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &benchmark_stop);
  benchmark_time =
      ((double)benchmark_stop.tv_sec + 1.0e-9 * benchmark_stop.tv_nsec) -
      ((double)benchmark_start.tv_sec + 1.0e-9 * benchmark_start.tv_nsec);
  printf("IPSET: %d IPs were analyzed in %f s.\n", SAMPLE_SIZE, benchmark_time);
  printf("One IP is analyzed in %g s\n", benchmark_time / SAMPLE_SIZE);
  printf("Found %d matches.\n", ip_set_sum);

  // Recover hipermap database from hm_db_place.
  char *hm_db2_place = malloc(hm_used);
  memcpy(hm_db2_place, hm_db_place, hm_used);
  free(hm_db_place0);
  hm_database_t *hm_db2;
  hm_err = hm_db_from_place(hm_db2_place, hm_used, &hm_db2);
  if (hm_err != HM_SUCCESS) {
    printf("hm_compile failed: %d.\n", hm_err);
    return 1;
  }

  clock_gettime(CLOCK_MONOTONIC, &benchmark_start);
  ip = 1;
  int hipermap_sum = 0;
  for (int i = 0; i < SAMPLE_SIZE; i++) {
    ip = hash32(ip);
    uint64_t hm_res = hm_find(hm_db2, ip);
    if (hm_res != HM_NO_VALUE) {
      hipermap_sum++;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &benchmark_stop);
  benchmark_time =
      ((double)benchmark_stop.tv_sec + 1.0e-9 * benchmark_stop.tv_nsec) -
      ((double)benchmark_start.tv_sec + 1.0e-9 * benchmark_start.tv_nsec);
  printf("HIPERMAP: %d IPs were analyzed in %f s.\n", SAMPLE_SIZE,
         benchmark_time);
  printf("One IP is analyzed in %g s\n", benchmark_time / SAMPLE_SIZE);
  printf("Found %d matches.\n", hipermap_sum);

  hs_err = hs_free_scratch(scratch);
  if (hs_err != HS_SUCCESS) {
    printf("hs_free_scratch failed: %d.\n", hs_err);
    return 1;
  }

  hs_err = hs_free_database(db);
  if (hs_err != HS_SUCCESS) {
    printf("hs_free_database failed: %d.\n", hs_err);
    return 1;
  }

  ipset_free(ip_set);
}
