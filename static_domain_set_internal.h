// Internal runtime layout shared between C and C++ translation units.
// Not part of the public API; do not include from user code.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef NDEBUG
#define debugf(fmt, ...) \
  do {                   \
  } while (0)
#else
#include <stdio.h>
#define debugf(...) fprintf(stderr, __VA_ARGS__)
#endif

// D is the number of domains per record.
#ifndef D
#define D 16
#endif

// MAX_DOMAIN_LEN is maximum allowed length of a domain name in bytes.
// Zero byte and final '.' are not included.
#ifndef MAX_DOMAIN_LEN
#define MAX_DOMAIN_LEN 253
#endif

// Define struct with alignment in a portable way across C and C++.
typedef struct domains_table_record {
  // domains_hashes has hash2's (16-bit tags) of all domains in this record.
  // The same hash can be included multiple times if there are collisions of
  // tags in the group represented by this domains_table_record.
  uint16_t domains_hashes[D];

  // domains_offsets stores offsets in units of D bytes from the base.
  // After each domain there is at least one 0x00 byte which can be used to
  // find its end. After last domain in the blob there is at least
  // MAX_DOMAIN_LEN bytes of allocated space, so it is safe to test that byte.
  uint8_t domains_offsets[D];

  // domains_blob is where the first domain of this record is stored in the
  // domain blob.
  const char* domains_blob;

  // used_slots stores how many slots are used in this record. Empty slots
  // are always located at the end of domains_hashes.
  uint16_t used_slots;

  // max_scans is maximum number of scan_tags runs needed to find all items
  // stored in this bucket. For a regular xxx.yy domain which is not popular
  // this number is 1. For domain like aaa.xxx.yy it is 2. If there is a popular
  // prefix co.uk, then aaa.co.uk has depth 1, bbb.aaa.co.uk has 2. This value
  // is used to limit the number of scan_tags runs inside the bucket digging
  // part of hm_domain_find and prevent slowness for pathological hosts like
  // "a.b.c.d.e.f. <very long list>.zz.yy>".
  uint16_t max_scans;

  // domains_blob_offset = domains_blob - db.domains_blob.
  uint32_t domains_blob_offset;
} domains_table_record_t;

// hm_domain_database is the in-memory layout used by the runtime.
typedef struct hm_domain_database {
  // fastmod_M is the precomputed parameter for fast modulus. It depends on
  // buckets.
  uint64_t fastmod_M;

  // buckets is the total number of records (buckets).
  uint32_t buckets;

  // hash_seed is mixed into hashing so we can reshuffle at compile-time.
  uint32_t hash_seed;

  // domains_table is the hash table (array of 64-byte records).
  domains_table_record_t* domains_table;

  // popular_table holds popular suffixes using the same 64-byte record layout
  // as domains_table. Suffix strings are stored at the beginning of
  // domains_blob. We keep the number of records and total suffix count.
  domains_table_record_t* popular_table;
  uint32_t popular_records;
  uint32_t popular_count;

  // domains_blob is concatenated, D-byte-aligned strings storage.
  char* domains_blob;
  size_t domains_blob_size;
} hm_domain_database_t;

// Portable "find last set bit" helpers, shared.
static inline int hm_fls32(uint32_t x) {
#if defined(_MSC_VER)
  unsigned long idx;
  return _BitScanReverse(&idx, x) ? (int)idx + 1 : 0;
#else
  return x ? 32 - __builtin_clz(x) : 0;
#endif
}

// Naive variant for non-find contexts: scan backwards for '.'
static inline const char* cut_last_domain_label_naive(const char* s,
                                                      const char* e) {
  if (s >= e) {
    return s;
  }
  const char* p = e;
  while (p > s) {
    --p;
    if (*p == '.') {
      return p + 1;
    }
  }
  return s;
}
