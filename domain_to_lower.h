#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(HM_DISABLE_SIMD) && defined(__AVX512BW__)
#include <immintrin.h>
#elif !defined(HM_DISABLE_SIMD) && defined(__AVX2__)
#include <immintrin.h>
#elif !defined(HM_DISABLE_SIMD) && (defined(__aarch64__) || defined(__ARM_NEON))
#include <arm_neon.h>
#endif

// Unaligned‑safe copy helpers for small tails.
static inline void hm_copy_16_tail(void* dst, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  if (n >= 8) {
    __builtin_memcpy(d, s, 8);
    s += 8;
    d += 8;
    n -= 8;
  }
  if (n >= 4) {
    __builtin_memcpy(d, s, 4);
    s += 4;
    d += 4;
    n -= 4;
  }
  if (n >= 2) {
    __builtin_memcpy(d, s, 2);
    s += 2;
    d += 2;
    n -= 2;
  }
  if (n) {
    *d = *s;
  }
}

static inline void hm_copy_32_tail(void* dst, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  if (n >= 16) {
    __builtin_memcpy(d, s, 16);
    s += 16;
    d += 16;
    n -= 16;
  }
  if (n) {
    hm_copy_16_tail(d, s, n);
  }
}

// Provides a fast ASCII domain lowercase + validate routine.
// Input:
//   - src pointer, dst pointer, length in bytes.
// Output:
//   - returns true iff all bytes are in [A-Za-z0-9-._].
// Side effects and contract:
//   - Writes lowercased bytes into dst (ASCII fold via OR 0x20).
//   - dst buffer MUST be at least 256 bytes; the function may store past
//   dst[len]
//     up to the SIMD register width (32 or 64 bytes) for the tail. It will not
//     write beyond dst[256). This avoids extra memcpy/branching in the tail.
static inline bool domain_to_lower(char* dst, const char* src, size_t len) {
#if defined(__AVX512BW__)
  const __m512i vspace = _mm512_set1_epi8(0x20);
  size_t i = 0;
  for (; i + 64 <= len; i += 64) {
    __m512i v = _mm512_loadu_si512((const void*)(src + i));
    // Classify bytes
    __mmask64 ge_0 = _mm512_cmpgt_epi8_mask(v, _mm512_set1_epi8('0' - 1));
    __mmask64 le_9 = _mm512_cmpgt_epi8_mask(_mm512_set1_epi8('9' + 1), v);
    __mmask64 is_digit = ge_0 & le_9;

    // Lowered view and alpha classification on lowered lanes only.
    __m512i vlo = _mm512_or_si512(v, vspace);
    __mmask64 ge_a = _mm512_cmpgt_epi8_mask(vlo, _mm512_set1_epi8('a' - 1));
    __mmask64 le_z = _mm512_cmpgt_epi8_mask(_mm512_set1_epi8('z' + 1), vlo);
    __mmask64 is_alpha = ge_a & le_z;

    __mmask64 is_dash = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('-'));
    __mmask64 is_dot = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('.'));
    __mmask64 is_us = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('_'));

    __mmask64 valid = is_digit | is_alpha | is_dash | is_dot | is_us;
    if (valid != ~(__mmask64)0) {
      return false;
    }

    // Lowercase only alphabetic bytes; keep others unchanged (including '_').
    __m512i out = _mm512_mask_mov_epi8(v, is_alpha, vlo);
    _mm512_storeu_si512((void*)(dst + i), out);
  }
  // Tail (<=63 bytes): masked load directly from src and store full 64B to dst.
  if (i < len) {
    size_t rem = len - i;
    __mmask64 need = ((__mmask64)1ULL << rem) - 1ULL;
    __m512i v = _mm512_maskz_loadu_epi8(need, (const void*)(src + i));
    __mmask64 ge_0 = _mm512_cmpgt_epi8_mask(v, _mm512_set1_epi8('0' - 1));
    __mmask64 le_9 = _mm512_cmpgt_epi8_mask(_mm512_set1_epi8('9' + 1), v);
    __mmask64 is_digit = ge_0 & le_9;
    __m512i vlo = _mm512_or_si512(v, vspace);
    __mmask64 ge_a = _mm512_cmpgt_epi8_mask(vlo, _mm512_set1_epi8('a' - 1));
    __mmask64 le_z = _mm512_cmpgt_epi8_mask(_mm512_set1_epi8('z' + 1), vlo);
    __mmask64 is_alpha = ge_a & le_z;
    __mmask64 is_dash = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('-'));
    __mmask64 is_dot = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('.'));
    __mmask64 is_us = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('_'));
    __mmask64 valid = is_digit | is_alpha | is_dash | is_dot | is_us;
    if ((valid & need) != need) {
      return false;
    }
    __m512i out = _mm512_mask_mov_epi8(v, is_alpha, vlo);
    _mm512_storeu_si512((void*)(dst + i), out);
  }
  return true;

#elif defined(__AVX2__)
  const __m256i vspace = _mm256_set1_epi8(0x20);
  size_t i = 0;
  for (; i + 32 <= len; i += 32) {
    __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)(src + i));

    __m256i vlo = _mm256_or_si256(v, vspace);
    __m256i ge_a = _mm256_cmpgt_epi8(vlo, _mm256_set1_epi8('a' - 1));
    __m256i le_z = _mm256_cmpgt_epi8(_mm256_set1_epi8('z' + 1), vlo);
    __m256i is_alpha = _mm256_and_si256(ge_a, le_z);

    __m256i ge_0 = _mm256_cmpgt_epi8(v, _mm256_set1_epi8('0' - 1));
    __m256i le_9 = _mm256_cmpgt_epi8(_mm256_set1_epi8('9' + 1), v);
    __m256i is_digit = _mm256_and_si256(ge_0, le_9);

    __m256i is_dash = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('-'));
    __m256i is_dot = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('.'));
    __m256i is_us = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('_'));

    __m256i valid = _mm256_or_si256(
        is_digit, _mm256_or_si256(_mm256_or_si256(is_dash, is_dot),
                                  _mm256_or_si256(is_alpha, is_us)));

    uint32_t mask = (uint32_t)_mm256_movemask_epi8(valid);
    if (mask != 0xFFFFFFFFu) {
      return false;
    }
    __m256i out = _mm256_blendv_epi8(v, vlo, is_alpha);
    _mm256_storeu_si256((__m256i*)(void*)(dst + i), out);
  }
  if (i < len) {
    size_t rem = len - i;
    char tmp[32] = {0};
    hm_copy_32_tail(tmp, src + i, rem);
    __m256i v = _mm256_loadu_si256((const __m256i*)(const void*)tmp);

    __m256i vlo = _mm256_or_si256(v, vspace);
    __m256i ge_a = _mm256_cmpgt_epi8(vlo, _mm256_set1_epi8('a' - 1));
    __m256i le_z = _mm256_cmpgt_epi8(_mm256_set1_epi8('z' + 1), vlo);
    __m256i is_alpha = _mm256_and_si256(ge_a, le_z);

    __m256i ge_0 = _mm256_cmpgt_epi8(v, _mm256_set1_epi8('0' - 1));
    __m256i le_9 = _mm256_cmpgt_epi8(_mm256_set1_epi8('9' + 1), v);
    __m256i is_digit = _mm256_and_si256(ge_0, le_9);

    __m256i is_dash = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('-'));
    __m256i is_dot = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('.'));
    __m256i is_us = _mm256_cmpeq_epi8(v, _mm256_set1_epi8('_'));

    __m256i valid = _mm256_or_si256(
        is_digit, _mm256_or_si256(_mm256_or_si256(is_dash, is_dot),
                                  _mm256_or_si256(is_alpha, is_us)));

    uint32_t mask = (uint32_t)_mm256_movemask_epi8(valid);
    uint32_t need = ((1u << rem) - 1u);
    if ((mask & need) != need) {
      return false;
    }
    __m256i out = _mm256_blendv_epi8(v, vlo, is_alpha);
    _mm256_storeu_si256((__m256i*)(void*)(dst + i), out);
  }
  return true;

#elif defined(__aarch64__) || defined(__ARM_NEON)
  const uint8x16_t vspace = vdupq_n_u8(0x20);
  size_t i = 0;
  for (; i + 16 <= len; i += 16) {
    uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)(src + i));

    uint8x16_t vlo = vorrq_u8(v, vspace);
    uint8x16_t ge_a = vcgeq_u8(vlo, vdupq_n_u8('a'));
    uint8x16_t le_z = vcleq_u8(vlo, vdupq_n_u8('z'));
    uint8x16_t is_alpha = vandq_u8(ge_a, le_z);

    uint8x16_t ge_0 = vcgeq_u8(v, vdupq_n_u8('0'));
    uint8x16_t le_9 = vcleq_u8(v, vdupq_n_u8('9'));
    uint8x16_t is_digit = vandq_u8(ge_0, le_9);

    uint8x16_t is_dash = vceqq_u8(v, vdupq_n_u8('-'));
    uint8x16_t is_dot = vceqq_u8(v, vdupq_n_u8('.'));
    uint8x16_t is_us = vceqq_u8(v, vdupq_n_u8('_'));

    uint8x16_t valid = vorrq_u8(is_digit, vorrq_u8(vorrq_u8(is_dash, is_dot),
                                                   vorrq_u8(is_alpha, is_us)));

    // Validate: all 16 lanes must be 0xFF (check 64-bit halves in-register).
    uint64x2_t halves = vreinterpretq_u64_u8(valid);
    if (vgetq_lane_u64(halves, 0) != ~UINT64_C(0) ||
        vgetq_lane_u64(halves, 1) != ~UINT64_C(0)) {
      return false;
    }

    uint8x16_t out = vbslq_u8(is_alpha, vlo, v);
    vst1q_u8((uint8_t*)(void*)(dst + i), out);
  }
  if (i < len) {
    size_t rem = len - i;
    uint8_t tmp[16] = {0};
    hm_copy_16_tail(tmp, src + i, rem);
    uint8x16_t v = vld1q_u8((const uint8_t*)(const void*)tmp);

    uint8x16_t vlo = vorrq_u8(v, vspace);
    uint8x16_t ge_a = vcgeq_u8(vlo, vdupq_n_u8('a'));
    uint8x16_t le_z = vcleq_u8(vlo, vdupq_n_u8('z'));
    uint8x16_t is_alpha = vandq_u8(ge_a, le_z);

    uint8x16_t ge_0 = vcgeq_u8(v, vdupq_n_u8('0'));
    uint8x16_t le_9 = vcleq_u8(v, vdupq_n_u8('9'));
    uint8x16_t is_digit = vandq_u8(ge_0, le_9);

    uint8x16_t is_dash = vceqq_u8(v, vdupq_n_u8('-'));
    uint8x16_t is_dot = vceqq_u8(v, vdupq_n_u8('.'));
    uint8x16_t is_us = vceqq_u8(v, vdupq_n_u8('_'));

    uint8x16_t valid = vorrq_u8(is_digit, vorrq_u8(vorrq_u8(is_dash, is_dot),
                                                   vorrq_u8(is_alpha, is_us)));

    // Validate only the first rem bytes using a 16-bit mask.
    uint8x16_t s = vshrq_n_u8(valid, 7);  // 0xFF -> 0x01, 0x00 -> 0x00
    uint8x8_t slo = vget_low_u8(s);
    uint8x8_t shi = vget_high_u8(s);
    const int8x8_t sh = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8x8_t wlo = vshl_u8(slo, sh);
    uint8x8_t whi = vshl_u8(shi, sh);
    uint32_t mask16 = (uint32_t)vaddv_u8(wlo) | ((uint32_t)vaddv_u8(whi) << 8);
    uint32_t need = (rem >= 16) ? 0xFFFFu : ((1u << rem) - 1u);
    if ((mask16 & need) != need) {
      return false;
    }

    uint8x16_t out = vbslq_u8(is_alpha, vlo, v);
    vst1q_u8((uint8_t*)(void*)(dst + i), out);
  }
  return true;

#else
  // Scalar fallback: classify alpha on lowered view; others on original.
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)src[i];
    unsigned char cl = (unsigned char)(c | 0x20);
    bool is_alpha = (cl >= 'a' && cl <= 'z');
    bool ok = is_alpha || (c >= '0' && c <= '9') || (c == '-') || (c == '.') ||
              (c == '_');
    if (!ok) {
      return false;
    }
    dst[i] = is_alpha ? (char)cl : (char)c;
  }
  return true;
#endif
}
