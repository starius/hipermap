#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cache.h"

const size_t alignment = 8;

char *align8(char *addr) {
  return (char *)(((uintptr_t)(addr) & ~(alignment - 1)) + alignment);
}

void fill_ip(char *bytes, uint32_t ip) {
  bytes[0] = (ip >> 24) & 0xFF;
  bytes[1] = (ip >> 16) & 0xFF;
  bytes[2] = (ip >> 8) & 0xFF;
  bytes[3] = ip & 0xFF;
}

void print_ip(uint32_t ip) {
  unsigned char bytes[4];
  fill_ip(bytes, ip);
  printf("%d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]);
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

int main(int argc, char *argv[]) {
  const unsigned int capacity = 128;

  size_t cache_place_size;
  hm_error_t hm_err = hm_cache_place_size(&cache_place_size, capacity);
  if (hm_err != HM_SUCCESS) {
    printf("hm_cache_place_size failed: %d.\n", hm_err);
    return 1;
  }

  char *cache_place0 = malloc(cache_place_size + alignment);
  char *cache_place = align8(cache_place0);

  hm_cache_t *cache;
  hm_err = hm_cache_init(cache_place, cache_place_size, &cache, capacity);
  if (hm_err != HM_SUCCESS) {
    printf("hm_cache_init failed: %d.\n", hm_err);
    return 1;
  }

  uint32_t ip = 1;
  const int SAMPLE_SIZE = 100000000;

  for (int i = 0; i < SAMPLE_SIZE; i++) {
    ip = hash32(ip);

    print_ip(ip);

    bool existed, evicted;
    uint32_t evicted_ip = 0, evicted_value = 0;
    hm_cache_add(cache, ip, i, &existed, &evicted, &evicted_ip, &evicted_value);

    printf(". existed=%b, evicted=%b, evicted_ip=", existed, evicted);
    print_ip(evicted_ip);
    printf(", evicted_value=%d\n", evicted_value);
  }

  free(cache_place0);
}
