// Own header - first, to make sure it is self-sufficient.
#include "static_domain_set.h"

// C headers.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(HM_DISABLE_SIMD) && defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#elif !defined(HM_DISABLE_SIMD) && (defined(__aarch64__) || defined(__ARM_NEON))
#include <arm_neon.h>
#endif

#include "cvendor/fastmod.h"
#include "domain_hash.h"
#include "domain_to_lower.h"
#include "static_domain_set_internal.h"

#ifdef NDEBUG
#define debugf(fmt, ...) \
  do {                   \
  } while (0)
#else
#include <stdio.h>
#define debugf(...) fprintf(stderr, __VA_ARGS__)
#endif

// Vector zero helpers for left and right paddings (32 bytes).
#if !defined(HM_DISABLE_SIMD) && defined(__AVX2__)
static inline void zero_pad_32(char* p) {
  __m128i z = _mm_setzero_si128();
  _mm_storeu_si128((__m128i*)(void*)(p + 0), z);
  _mm_storeu_si128((__m128i*)(void*)(p + 16), z);
}
#elif !defined(HM_DISABLE_SIMD) && (defined(__aarch64__) || defined(__ARM_NEON))
static inline void zero_pad_32(char* p) {
  uint8x16_t z = vdupq_n_u8(0);
  vst1q_u8((uint8_t*)(void*)(p + 0), z);
  vst1q_u8((uint8_t*)(void*)(p + 16), z);
}
#else
static inline void zero_pad_32(char* p) { memset(p, 0, 32); }
#endif

// Fast equality compare using 16-byte lanes; treats bytes past 'n' as don't
// care in the final chunk (requires 16 bytes of readable padding). Assumes
// that 'b' is 16-byte aligned (domains blob is D=16 aligned and offsets are
// in units of D), while 'a' may be unaligned.
#if defined(__AVX2__)
static inline bool compare_domains_eq(const char* a, const char* b, size_t n) {
  if (n == 0) {
    return true;
  }
  size_t blocks = (n + 15) >> 4;  // ceil(n/16)
  size_t full = (blocks - 1) << 4;
  for (size_t i = 0; i < full; i += 16) {
    __m128i va = _mm_loadu_si128((const __m128i*)(const void*)(a + i));
    __m128i vb = _mm_load_si128((const __m128i*)(const void*)(b + i));
    __m128i eq = _mm_cmpeq_epi8(va, vb);
    if (_mm_movemask_epi8(eq) != 0xFFFF) {
      return false;
    }
  }
  // Last (possibly partial) block: mask insignificant bytes.
  size_t rem = n - full;  // 1..16
  __m128i va = _mm_loadu_si128((const __m128i*)(const void*)(a + full));
  __m128i vb = _mm_load_si128((const __m128i*)(const void*)(b + full));
  __m128i eq = _mm_cmpeq_epi8(va, vb);
  unsigned need = (rem == 16) ? 0xFFFFu : ((1u << rem) - 1u);
  unsigned got = (unsigned)_mm_movemask_epi8(eq) & need;
  return got == need;
}
#elif defined(__aarch64__) || defined(__ARM_NEON)
static inline bool compare_domains_eq(const char* a, const char* b, size_t n) {
  if (n == 0) {
    return true;
  }
  size_t blocks = (n + 15) >> 4;  // ceil(n/16)
  size_t full = (blocks - 1) << 4;
  for (size_t i = 0; i < full; i += 16) {
    uint8x16_t va = vld1q_u8((const uint8_t*)(const void*)(a + i));
    uint8x16_t vb = vld1q_u8((const uint8_t*)(const void*)(b + i));
    uint8x16_t eq = vceqq_u8(va, vb);
    uint64x2_t halves = vreinterpretq_u64_u8(eq);
    if (vgetq_lane_u64(halves, 0) != ~UINT64_C(0) ||
        vgetq_lane_u64(halves, 1) != ~UINT64_C(0)) {
      return false;
    }
  }
  size_t rem = n - full;  // 1..16
  uint8x16_t va = vld1q_u8((const uint8_t*)(const void*)(a + full));
  uint8x16_t vb = vld1q_u8((const uint8_t*)(const void*)(b + full));
  uint8x16_t eq = vceqq_u8(va, vb);
  // Turn per-byte 0xFF/0x00 into 1/0 bits, then accumulate into 16-bit mask
  uint8x16_t s = vshrq_n_u8(eq, 7);
  uint8x8_t slo = vget_low_u8(s);
  uint8x8_t shi = vget_high_u8(s);
  const int8x8_t sh = {0, 1, 2, 3, 4, 5, 6, 7};
  uint8x8_t wlo = vshl_u8(slo, sh);
  uint8x8_t whi = vshl_u8(shi, sh);
  unsigned mask16 = (unsigned)vaddv_u8(wlo) | ((unsigned)vaddv_u8(whi) << 8);
  unsigned need = (rem == 16) ? 0xFFFFu : ((1u << rem) - 1u);
  return (mask16 & need) == need;
}
#else
static inline bool compare_domains_eq(const char* a, const char* b, size_t n) {
  return memcmp(a, b, n) == 0;
}
#endif

#if defined(__AVX2__)
// Optimized for hm_domain_find: assumes there are at least 16 bytes of readable
// padding before 'start' (we zero-fill it in the caller) so wide loads never
// segfault. We still guard against returning a position in the padding by
// checking the found '.' is >= start. Returns beginning of the last label
// (dot+1), or 'start' if no dot in [start, end).
static inline const char* cut_last_domain_label_fast(const char* start,
                                                     const char* end) {
  // Start of the pad.
  const char* minp = (const char*)start - 16;

  const __m128i needle = _mm_set1_epi8('.');

  for (const char* cur = end - 16; cur > minp; cur -= 16) {
    __m128i v = _mm_loadu_si128((const __m128i*)cur);
    __m128i eq = _mm_cmpeq_epi8(v, needle);
    uint32_t mask = (uint32_t)_mm_movemask_epi8(eq);
    if (mask) {
      int msb_index = hm_fls32(mask) - 1;  // highest set bit index (0..15)
      const char* dot = (const char*)cur + msb_index;
      if (dot >= start) {
        return dot + 1;
      } else {
        break;  // match lies in the padding, stop
      }
    }
  }
  return start;
}

// Return beginning of the last two-label suffix, optimized to detect two
// dots within the same 32-byte window when possible.
static inline const char* cut_two_last_domain_labels_fast(const char* start,
                                                          const char* end) {
  const char* cur = (const char*)end - 32;
  const char* minp = (const char*)start - 32;

  const __m256i needle = _mm256_set1_epi8('.');

  // Phase 1: find the rightmost '.'; try to find the second in the same block
  for (; cur > minp; cur -= 32) {
    debugf("[cut2] load32 phase1 start=%p end=%p minp=%p cur=%p\n",
           (void*)start, (void*)end, (void*)minp, (void*)cur);
    __m256i v = _mm256_loadu_si256((const __m256i*)cur);
    __m256i eq = _mm256_cmpeq_epi8(v, needle);
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);
    if (!mask) {
      continue;
    }
    int msb = hm_fls32(mask) - 1;
    const char* dot1 = (const char*)cur + msb;
    if (dot1 < start) {
      return start;  // all earlier bits/blocks are in padding too
    }
    mask &= ~(1u << msb);
    if (!mask) {
      // There is just one '.' in this 32 byte block. Switch to phase 2.
      cur -= 32;
      break;
    }
    int msb2 = hm_fls32(mask) - 1;
    const char* dot2 = (const char*)cur + msb2;
    if (dot2 >= start) {
      return dot2 + 1;
    }
    // Second lies in padding; earlier blocks are further left => also padding.
    return start;
  }

  // Phase 2: find the next '.' in earlier blocks.
  for (; cur > minp; cur -= 32) {
    debugf("[cut2] load32 phase2 start=%p end=%p minp=%p cur=%p\n",
           (void*)start, (void*)end, (void*)minp, (void*)cur);
    __m256i v = _mm256_loadu_si256((const __m256i*)cur);
    __m256i eq = _mm256_cmpeq_epi8(v, needle);
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);
    if (!mask) {
      continue;
    }
    int msb2 = hm_fls32(mask) - 1;
    const char* dot2 = (const char*)cur + msb2;
    if (dot2 >= start) {
      return dot2 + 1;
    } else {
      return start;
    }
  }
  return start;
}
#elif defined(__aarch64__) || defined(__ARM_NEON)
// NEON-accelerated cutters using 16-byte block scans and bit masks.
static inline uint32_t neon_movemask_u8x16(uint8x16_t v) {
  // Move MSB to LSB for each byte and build 16-bit mask using per-lane shifts.
  uint8x16_t s = vshrq_n_u8(v, 7);
  uint8x8_t slo = vget_low_u8(s);
  uint8x8_t shi = vget_high_u8(s);
  const int8x8_t sh = {0, 1, 2, 3, 4, 5, 6, 7};
  uint8x8_t wlo = vshl_u8(slo, sh);
  uint8x8_t whi = vshl_u8(shi, sh);
  uint8_t lo = vaddv_u8(wlo);
  uint8_t hi = vaddv_u8(whi);
  return (uint32_t)lo | ((uint32_t)hi << 8);
}

static inline const char* cut_last_domain_label_fast(const char* start,
                                                     const char* end) {
  const uint8_t* minp = (const uint8_t*)start - 16;

  const uint8x16_t needle = vdupq_n_u8((uint8_t)'.');

  for (const uint8_t* cur = (const uint8_t*)end - 16; cur > minp; cur -= 16) {
    uint8x16_t v = vld1q_u8(cur);
    uint32_t mask16 = neon_movemask_u8x16(vceqq_u8(v, needle));
    if (!mask16) {
      continue;
    }
    int msb = hm_fls32(mask16) - 1;  // highest set bit 0..15
    const char* dot = (const char*)(cur + msb);
    if (dot >= start) {
      return dot + 1;
    } else {
      return start;  // match in padding
    }
  }
  return start;
}

static inline const char* cut_two_last_domain_labels_fast(const char* start,
                                                          const char* end) {
  const uint8_t* cur = (const uint8_t*)end - 16;
  const uint8_t* minp = (const uint8_t*)start - 16;

  const uint8x16_t needle = vdupq_n_u8((uint8_t)'.');

  // Phase 1: find rightmost '.' and try to find the second within the block.
  for (; cur > minp; cur -= 16) {
    uint32_t mask16 = neon_movemask_u8x16(vceqq_u8(vld1q_u8(cur), needle));
    if (!mask16) {
      continue;
    }
    int msb = hm_fls32(mask16) - 1;
    const char* dot1 = (const char*)(cur + msb);
    if (dot1 < start) {
      return start;  // in padding
    }
    mask16 &= ~(1u << msb);
    if (mask16) {
      int msb2 = hm_fls32(mask16) - 1;
      const char* dot2 = (const char*)(cur + msb2);
      return (dot2 >= start) ? (dot2 + 1) : start;
    }
    // None in the same block; proceed to phase 2 to earlier blocks.
    cur -= 16;
    break;
  }

  // Phase 2: continue scanning earlier blocks for the second '.'
  for (; cur > minp; cur -= 16) {
    uint32_t mask16 = neon_movemask_u8x16(vceqq_u8(vld1q_u8(cur), needle));
    if (!mask16) {
      continue;
    }
    int msb2 = hm_fls32(mask16) - 1;
    const char* dot2 = (const char*)(cur + msb2);
    return (dot2 >= start) ? (dot2 + 1) : start;
  }
  return start;
}

// NEON compare of 16x16-bit lanes producing a 16-bit bitfield.
static inline uint32_t neon_movemask_from_u16eq(uint16x8_t e0, uint16x8_t e1) {
  // 0xFFFF/0x0000 -> 0x01/0x00 bytes per lane, then shift by lane index.
  uint8x8_t b0 = vmovn_u16(vshrq_n_u16(e0, 15));
  uint8x8_t b1 = vmovn_u16(vshrq_n_u16(e1, 15));
  const int8x8_t sh = (int8x8_t){0, 1, 2, 3, 4, 5, 6, 7};
  uint8x8_t w0 = vshl_u8(b0, sh);
  uint8x8_t w1 = vshl_u8(b1, sh);
  uint8_t lo = vaddv_u8(w0);
  uint8_t hi = vaddv_u8(w1);
  return (uint32_t)lo | ((uint32_t)hi << 8);
}
#else
static inline const char* cut_last_domain_label_fast(const char* start,
                                                     const char* end) {
  return cut_last_domain_label_naive(start, end);
}
static inline const char* cut_two_last_domain_labels_fast(const char* start,
                                                          const char* end) {
  const char* p1 = cut_last_domain_label_naive(start, end);
  if (p1 > start) {
    return cut_last_domain_label_naive(start, p1 - 1);
  }
  return start;
}
#endif

// scan_tags searches for `tag` within the record's 16-bit tag array, limited
// to `used_slots`. On each match, it computes a pointer into the domains blob
// for that slot and compares it with suffix as C string (0x00 terminated). It
// returns if there is a match. Inclusion of that 0x00 is needed to avoid false
// positives in case the domain in the blob is longer.
static inline bool scan_tags(const domains_table_record_t* rec, uint16_t tag,
                             const char* suffix, size_t suffix_len) {
  uint32_t used = rec->used_slots;
  if (used == 0) {
    return false;
  }

  const uint16_t* tags = rec->domains_hashes;
  const char* base = rec->domains_blob;

#if defined(__AVX2__)
  // Compare all 16 lanes at once.
  __m256i needle = _mm256_set1_epi16((short)tag);
  __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)tags);
  __m256i eq = _mm256_cmpeq_epi16(v, needle);
  uint32_t mm = (uint32_t)_mm256_movemask_epi8(eq);
  uint32_t pair = mm & (mm >> 1);

  // Each tag is represented with two bits (00 or 01) in pair, starting at the
  // least significant bit position. We need to zero bits which are not covered
  // by rec->used_slots.
  uint32_t limit_bits = used * 2u;
  // Build mask using 64-bit shift to safely handle limit_bits==32.
  // Example: limit_bits=32 => ((1ULL<<32)-1) == 0xFFFF'FFFF.
  uint32_t mask = (uint32_t)(((uint64_t)1ULL << limit_bits) - 1ULL);
  pair &= mask;

  debugf("[scan]     used=%u tag=0x%04x mm=%08x pair=%08x mask=%08x\n", used,
         (unsigned)tag, mm, pair, mask);

  while (pair) {
    uint32_t bit = __builtin_ctz(pair);
    uint32_t i = bit >> 1;  // lane index 0..15
    const char* cand = base + (size_t)(rec->domains_offsets[i]) * D;
    // Equality compare domain (including final 0x00) without calling memcmp.
    bool eq = compare_domains_eq(suffix, cand, suffix_len + 1);
    debugf("[scan]       lane=%u cand='%s' eq=%d\n", i, cand, (int)eq);
    if (eq) {
      return true;
    }

    // Zero the bit that we processed. E.g. 010100-1 is 010011.
    pair &= (pair - 1);
  }
#elif defined(__aarch64__) || defined(__ARM_NEON)
  uint16x8_t v0 = vld1q_u16((const uint16_t*)(const void*)tags);
  uint16x8_t v1 = vld1q_u16((const uint16_t*)(const void*)(tags + 8));
  uint16x8_t n = vdupq_n_u16((uint16_t)tag);
  uint32_t mask16 =
      neon_movemask_from_u16eq(vceqq_u16(v0, n), vceqq_u16(v1, n));
  // Limit to used slots
  uint32_t used_mask = (used >= 16) ? 0xFFFFu : ((1u << used) - 1u);
  mask16 &= used_mask;

  debugf("[scan]     used=%u tag=0x%04x neon_mask=%04x\n", used, (unsigned)tag,
         mask16);

  while (mask16) {
    uint32_t i = __builtin_ctz(mask16);  // lane index 0..15
    const char* cand = base + (size_t)(rec->domains_offsets[i]) * D;
    bool eq = compare_domains_eq(suffix, cand, suffix_len + 1);
    debugf("[scan]       lane=%u cand='%s' eq=%d\n", i, cand, (int)eq);
    if (eq) {
      return true;
    }
    mask16 &= (mask16 - 1);
  }
#else
  // Scalar fallback.
  for (uint32_t i = 0; i < used; i++) {
    if (tags[i] == tag) {
      const char* cand = base + (size_t)rec->domains_offsets[i] * D;
      // Compare the domain and its final 0x00 (hence suffix_len+1).
      if (memcmp(suffix, cand, suffix_len + 1) == 0) {
        return true;
      }
    }
  }
#endif

  return false;
}

// Check if the given suffix is present in the popular_table by scanning all
// short records with SIMD tag compare and memcmp on match.
static inline bool popular_suffix_exists(const hm_domain_database_t* db,
                                         uint16_t tag, const char* suffix,
                                         size_t suffix_len) {
  uint32_t rnum = db->popular_records;
  if (rnum == 0) {
    return false;
  }
  const domains_table_record_t* recs = db->popular_table;
  for (uint32_t r = 0; r < rnum; r++) {
    if (scan_tags(&recs[r], tag, suffix, suffix_len)) {
      return true;
    }
  }
  return false;
}

int HM_CDECL hm_domain_find(const hm_domain_database_t* db, const char* domain,
                            size_t domain_len) {
  debugf("[find] === query '%.*s' ===\n", (int)domain_len, domain);
  // compute on a lowercased copy below; pad 32 bytes before for AVX2 scans

  // Remove trailing '.'.
  while (domain_len > 0 && domain[domain_len - 1] == '.') {
    domain_len--;
  }

  // Make sure that the domain is not too long. This might cause a segfault
  // when comparing with domains from blob, if the compared domain in the bloc
  // is in the end. No domain in the blob is longer than MAX_DOMAIN_LEN
  // anyway.
  if (domain_len > MAX_DOMAIN_LEN || domain_len == 0) {
    debugf("[find] failure for domain '%.*s': len=%d\n", (int)domain_len,
           domain, (int)domain_len);
    return -1;
  }

  // Convert the domain to lowercase and validate it in one go.
  // 32 bytes of left padding for AVX/SSE cutters and 32 bytes of right
  // padding to allow final vector loads during equality checks without
  // branching.
  char lower_buf[256 + 32 + 32];
  char* domain_lower = lower_buf + 32;
  // Zero the 32-byte padding immediately preceding the domain start.
  zero_pad_32(lower_buf);
  if (!domain_to_lower(domain_lower, domain, domain_len)) {
    debugf("[find] failure for domain '%.*s': domain_to_lower\n",
           (int)domain_len, domain);
    return -1;
  }
  char* domain_end = domain_lower + domain_len;
  debugf("[find]   addr lower_buf(base)=%p domain_lower=%p end=%p len=%zu\n",
         (void*)(domain_lower - 32), (void*)domain_lower, (void*)domain_end,
         (size_t)domain_len);

  // Zero 32 bytes after the domain to be able to compare with the domain in
  // blob and to make tail vector loads safe for SIMD equality.
  zero_pad_32(domain_end);

  debugf("[find]   lower='%.*s'\n", (int)domain_len, domain_lower);

  // Cut last two components. If there is just one component, cut only it.
  const char* suffix =
      cut_two_last_domain_labels_fast(domain_lower, domain_end);

  // Hash two final components.
  uint64_t suffix_hash =
      hash64_span_ci(suffix, domain_end - suffix, db->hash_seed);

  debugf("[find]   initial suffix='%.*s' pop=0x%04x\n",
         (int)(domain_end - suffix), suffix, (unsigned)(suffix_hash & 0xFFFFu));

  // If the current suffix is popular (by exact-suffix match), extend left.
  while (suffix > domain_lower) {
    uint16_t pop_tag = (uint16_t)((suffix_hash >> 32) & 0xFFFFu);
    size_t sfx_len = (size_t)(domain_end - suffix);
    if (!popular_suffix_exists(db, pop_tag, suffix, sfx_len)) {
      break;
    }
    const char* label_end = suffix - 1;
    const char* label = cut_last_domain_label_fast(domain_lower, label_end);
    suffix_hash = hash64_span_ci(label, label_end - label, suffix_hash);
    suffix = label;
    debugf("[find]   extended popular suffix='%.*s' pop=0x%04x\n",
           (int)(domain_end - suffix), suffix,
           (unsigned)(suffix_hash & 0xFFFFu));
  }

  // Find the bucket as (uint32_t)suffix_hash % buckets. Use fastmod.
  uint32_t bucket_hash = (uint32_t)suffix_hash;
  uint32_t bucket = fastmod_u32(bucket_hash, db->fastmod_M, db->buckets);
  const domains_table_record_t* rec = &db->domains_table[bucket];
#ifndef NDEBUG
  debugf("[find]   bucket=%u used_slots=%u max_scans=%u\n", bucket,
         rec->used_slots, rec->max_scans);
  if (rec->used_slots > 0) {
    for (uint32_t i = 0; i < rec->used_slots; i++) {
      const char* cand =
          rec->domains_blob + (size_t)rec->domains_offsets[i] * D;
      debugf("[find]     rec.tag[%u]=0x%04x cand='%s'\n", i,
             (unsigned)rec->domains_hashes[i], cand);
    }
  }
#endif

  // Now check presence of the current prefix and all larger prefixes in the
  // bucket. We do not exceed max_scans not to vaste CPU cycles in case of
  // pathological domains with many short prefixes.
  int max_scans = (int)(rec->max_scans);
  for (int scan = 1;; scan++) {
    size_t suffix_len = domain_end - suffix;
    uint16_t tag = (uint16_t)((suffix_hash >> 32) & 0xFFFFu);
    debugf(
        "[find]   try scan=%d/%d suffix='%.*s' tag=0x%04x "
        "pop=0x%04x\n",
        scan, max_scans, (int)suffix_len, suffix, (unsigned)tag,
        (unsigned)(suffix_hash & 0xFFFFu));

    // Try to scan this suffix.
    if (scan_tags(rec, tag, suffix, suffix_len)) {
      debugf("[find]   match: '%.*s'\n", (int)suffix_len, suffix);
      return 1;
    }

    // If we reached max_scans inside the bucket, but haven't found the domain,
    // we can exit now. The passed domain is longer (in domain parts) then all
    // the domains in the bucket.
    if (scan >= max_scans) {
      debugf("[find]   max_scans exceeded\n");
      return 0;
    }

    // If suffix can't grow, stop. Nothing was found.
    if (suffix <= domain_lower) {
      return 0;
    }

    // Grow the suffix, update the hash.
    const char* label_end = suffix - 1;
    const char* label = cut_last_domain_label_fast(domain_lower, label_end);
    suffix_hash = hash64_span_ci(label, label_end - label, suffix_hash);
    suffix = label;
    debugf("[find]   grow suffix -> '%.*s'\n", (int)(domain_end - suffix),
           suffix);
  }
}
