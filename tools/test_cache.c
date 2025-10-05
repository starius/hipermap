#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
static int clock_gettime(int clk_id, struct timespec* tp) {
  (void)clk_id;
  static LARGE_INTEGER freq;
  static BOOL init = FALSE;
  LARGE_INTEGER counter;
  if (!init) {
    if (!QueryPerformanceFrequency(&freq)) {
      return -1;
    }
    init = TRUE;
  }
  if (!QueryPerformanceCounter(&counter)) {
    return -1;
  }
  tp->tv_sec = (time_t)(counter.QuadPart / freq.QuadPart);
  tp->tv_nsec = (long)((counter.QuadPart % freq.QuadPart) * 1000000000ULL /
                       freq.QuadPart);
  return 0;
}
#endif

#include "../cache.h"

void fill_ip(char* bytes, uint32_t ip) {
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

int main(int argc, char* argv[]) {
  const unsigned int capacity = 128;

  int speed = 3;

  size_t cache_place_size;
  hm_error_t hm_err = hm_cache_place_size(&cache_place_size, capacity, speed);
  if (hm_err != HM_SUCCESS) {
    printf("hm_cache_place_size failed: %d.\n", hm_err);
    return 1;
  }

  char* cache_place = malloc(cache_place_size);

  hm_cache_t* cache;
  hm_err =
      hm_cache_init(cache_place, cache_place_size, &cache, capacity, speed);
  if (hm_err != HM_SUCCESS) {
    printf("hm_cache_init failed: %d.\n", hm_err);
    return 1;
  }

  uint32_t ip = 1;
  const int SAMPLE_SIZE = 10000000;

  uint32_t* ips = malloc(sizeof(uint32_t) * capacity);

  struct timespec benchmark_start, benchmark_stop;
  double benchmark_time;

  clock_gettime(CLOCK_MONOTONIC, &benchmark_start);
  for (int i = 0; i < SAMPLE_SIZE; i++) {
    ip = hash32(ip);

    bool existed, evicted;
    uint32_t evicted_ip = 0, evicted_value = 0;
    hm_cache_add(cache, ip, i, &existed, &evicted, &evicted_ip, &evicted_value);

    // print_ip(ip);
    // printf(" (%d). existed=%b, evicted=%b, evicted_ip=", ip, existed,
    // evicted); print_ip(evicted_ip); printf(", evicted_value=%d\n",
    // evicted_value);

    size_t len = capacity;
    // hm_cache_dump(cache, ips, &len);
  }
  clock_gettime(CLOCK_MONOTONIC, &benchmark_stop);
  benchmark_time =
      ((double)benchmark_stop.tv_sec + 1.0e-9 * benchmark_stop.tv_nsec) -
      ((double)benchmark_start.tv_sec + 1.0e-9 * benchmark_start.tv_nsec);
  printf("%d IPs were added in %f s.\n", SAMPLE_SIZE, benchmark_time);
  printf("One IP was added in %g s\n", benchmark_time / SAMPLE_SIZE);

  free(cache_place);
}
