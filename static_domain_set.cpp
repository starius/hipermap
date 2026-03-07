// Own header - first, to make sure it is self-sufficient.
#include "static_domain_set.h"

// C includes.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

// C++ includes.
#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef __cplusplus
#error C++ required
#endif

#include "cvendor/fastmod.h"
#include "domain_hash.h"
#include "domain_to_lower.h"
#include "static_domain_set_internal.h"

// Popular suffixes hard cap kept in DB.
#define MAX_POPULAR_SUFFIXES 256

// Calibration/search constants (shared by size estimation and compile):
#define CALIB_GROW_STEPS 60   // number of growth steps
#define CALIB_SEED_TRIES 100  // seed trials per size
#define CALIB_GROW_NUM 21     // growth factor numerator (~ +5%)
#define CALIB_GROW_DEN 20     // growth factor denominator

// Memory padding constants:
#define SIMD_TAIL_PAD 64   // bytes to absorb SIMD tail loads
#define ALIGN_HEADROOM 64  // headroom to realign db_place base

#define STATIC_DOMAIN_SET_MAGIC_V1 0x53444D48u
#define STATIC_DOMAIN_SET_MAGIC_V2 0x32444D48u
#define SERIALIZED_HEADER_BYTES 64u

static_assert(sizeof(domains_table_record_t) == 64, "Struct must be 64 bytes");

// serialized_header_v2_t is the fixed-size on-disk header layout for
// serialization format v2.
struct serialized_header_v2_t {
  // fastmod_M is the precomputed multiplier for bucket modulus.
  uint64_t fastmod_M;

  // buckets is the number of main hash-table records.
  uint32_t buckets;

  // hash_seed is the compile-time selected seed for XXH3 chaining.
  uint32_t hash_seed;

  // popular_records is the number of records in popular_table.
  uint32_t popular_records;

  // popular_count is the number of popular suffix strings.
  uint32_t popular_count;

  // tld_records is the number of records in tld_table.
  uint32_t tld_records;

  // tld_count is the number of top-level domains stored in tld_table.
  uint32_t tld_count;

  // domains_blob_size is the size of serialized string storage in bytes.
  uint64_t domains_blob_size;

  // reserved keeps the v2 header fixed at 64 bytes for future extensions.
  uint8_t reserved[SERIALIZED_HEADER_BYTES - 40];
};

static_assert(sizeof(serialized_header_v2_t) == SERIALIZED_HEADER_BYTES,
              "v2 serialized header must be 64 bytes");

// serialized_meta_t stores normalized parsed metadata from v1 or v2 serialized
// bytes.
struct serialized_meta_t {
  // is_v2 indicates whether serialized bytes were parsed as v2 format.
  bool is_v2 = false;

  // fastmod_M is copied from header and used to restore runtime DB state.
  uint64_t fastmod_M = 0;

  // buckets is the number of main table records in serialized payload.
  uint32_t buckets = 0;

  // hash_seed is the seed used by hashing during lookup.
  uint32_t hash_seed = 0;

  // popular_records is the count of serialized popular records.
  uint32_t popular_records = 0;

  // popular_count is the number of popular suffix strings.
  uint32_t popular_count = 0;

  // tld_records is the count of serialized TLD records (v2 only).
  uint32_t tld_records = 0;

  // tld_count is the number of TLD strings (v2 only).
  uint32_t tld_count = 0;

  // blob_bytes is the size of serialized blob region.
  uint64_t blob_bytes = 0;
};

static inline char* align(char* addr, size_t alignment) {
  return (char*)(((uintptr_t)(addr) & ~(uintptr_t)(alignment - 1)) + alignment);
}

static inline size_t round_up16(size_t x) { return (x + 15) & ~15; }

static inline size_t round_up64(size_t x) { return (x + 63) & ~63; }

// add_size_overflow returns true if a+b would overflow size_t.
static inline bool add_size_overflow(size_t a, size_t b, size_t* out) {
  if (!out) {
    return true;
  }
  if (a > SIZE_MAX - b) {
    return true;
  }
  *out = a + b;
  return false;
}

// mul_size_overflow returns true if a*b would overflow size_t.
static inline bool mul_size_overflow(size_t a, size_t b, size_t* out) {
  if (!out) {
    return true;
  }
  if (a == 0 || b == 0) {
    *out = 0;
    return false;
  }
  if (a > SIZE_MAX / b) {
    return true;
  }
  *out = a * b;
  return false;
}

// write_u32 stores a little-endian uint32 into byte buffer p.
static inline void write_u32(char* p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

// write_u64 stores a little-endian uint64 into byte buffer p.
static inline void write_u64(char* p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

// read_u32 loads a little-endian uint32 from byte buffer p.
static inline uint32_t read_u32(const char* p) {
  uint32_t v = 0;
  memcpy(&v, p, sizeof(v));
  return v;
}

// read_u64 loads a little-endian uint64 from byte buffer p.
static inline uint64_t read_u64(const char* p) {
  uint64_t v = 0;
  memcpy(&v, p, sizeof(v));
  return v;
}

// parse_serialized_meta parses v1/v2 serialized headers into normalized
// metadata used by deserialization and size validation.
static inline bool parse_serialized_meta(const char* buffer, size_t buffer_size,
                                         serialized_meta_t* out) {
  if (!buffer || !out || buffer_size < 4 + SERIALIZED_HEADER_BYTES) {
    return false;
  }
  uint32_t magic = read_u32(buffer);
  const char* hdr = buffer + 4;
  if (magic == STATIC_DOMAIN_SET_MAGIC_V2) {
    out->is_v2 = true;
    out->fastmod_M = read_u64(hdr + 0);
    out->buckets = read_u32(hdr + 8);
    out->hash_seed = read_u32(hdr + 12);
    out->popular_records = read_u32(hdr + 16);
    out->popular_count = read_u32(hdr + 20);
    out->tld_records = read_u32(hdr + 24);
    out->tld_count = read_u32(hdr + 28);
    out->blob_bytes = read_u64(hdr + 32);
    return true;
  }
  if (magic == STATIC_DOMAIN_SET_MAGIC_V1) {
    // v1 header is hm_domain_database_t serialized as a 64-byte block.
    out->is_v2 = false;
    out->fastmod_M = read_u64(hdr + 0);
    out->buckets = read_u32(hdr + 8);
    out->hash_seed = read_u32(hdr + 12);
    out->popular_records = read_u32(hdr + 32);
    out->popular_count = read_u32(hdr + 36);
    out->tld_records = 0;
    out->tld_count = 0;
    out->blob_bytes = read_u64(hdr + 48);
    return true;
  }
  return false;
}

// less_rev_char compares two strings from the last character backwards.
// A shorter suffix-only string (e.g., "example.com") will precede its
// subdomains (e.g., "a.example.com"), which allows pruning by single pass.
static bool less_rev_char(const std::string_view& a,
                          const std::string_view& b) {
  return std::lexicographical_compare(a.rbegin(), a.rend(), b.rbegin(),
                                      b.rend());
}

// is_subdomain returns is s is a subdomain of suf or equal to suf.
static inline bool is_subdomain(const std::string_view& s,
                                const std::string_view& suf) {
  if (s.size() < suf.size()) {
    return false;
  }
  if (memcmp(s.data() + s.size() - suf.size(), suf.data(), suf.size()) != 0) {
    return false;
  }

  // Check if they are equal.
  if (s.size() == suf.size()) {
    return true;
  }

  return s[s.size() - suf.size() - 1] == '.';
}

// prune_subdomains_revchar removes subdomains when their base domain is
// present. It sorts by reversed character order, then keeps the first
// occurrence and drops consecutive entries that are suffixes on label
// boundaries.
static void prune_subdomains_revchar(std::vector<std::string_view>& domains) {
  if (domains.empty()) {
    return;
  }
  std::sort(domains.begin(), domains.end(), less_rev_char);
  std::vector<std::string_view> out;
  out.reserve(domains.size());
  for (const auto& s : domains) {
    if (out.empty()) {
      out.push_back(s);
      continue;
    }
    const std::string_view base = out.back();

    // If s is equal to base ot its subdomain, skip it.
    if (is_subdomain(s, base)) {
      continue;
    }
    out.push_back(s);
  }
  domains.swap(out);
}

// Preprocess input domains into views (lowercased when needed, trailing '.'
// stripped). Returns false if any domain is invalid.
static bool preprocess_domains_views(
    const char** domains, unsigned elements,
    std::list<std::string>& domains_lower,
    std::vector<std::string_view>& domains_views) {
  domains_views.clear();
  domains_views.reserve(elements);
  for (unsigned i = 0; i < elements; i++) {
    const char* start = domains[i];
    if (!start) {
      debugf("[preprocess] domains[%u]: null pointer\n", i);
      return false;
    }
    size_t len = strlen(start);
    const char* end = start + len;
    // Cut trailing '.'
    while (end > start && end[-1] == '.') {
      end--;
    }
    len = (size_t)(end - start);
    if (len == 0 || len > MAX_DOMAIN_LEN) {
      debugf("[preprocess] domains[%u]: invalid length=%zu (raw='%s')\n", i,
             len, start);
      return false;
    }
    char lower[256];
    if (!domain_to_lower(lower, start, len)) {
      debugf(
          "[preprocess] domains[%u]: domain_to_lower failed (len=%zu, "
          "raw='%.*s')\n",
          i, len, (int)len, start);
      return false;
    }
    if (memcmp(lower, start, len) == 0) {
      domains_views.emplace_back(start, len);
    } else {
      const std::string& s = domains_lower.emplace_back(lower, len);
      domains_views.emplace_back(s);
    }
  }
  prune_subdomains_revchar(domains_views);
  return true;
}

// split_regular_and_tld_domains separates input domains into multi-label
// regular domains and single-label top-level domains.
static void split_regular_and_tld_domains(
    const std::vector<std::string_view>& all_domains,
    std::vector<std::string_view>& regular_domains,
    std::vector<std::string_view>& tld_domains) {
  regular_domains.clear();
  tld_domains.clear();
  regular_domains.reserve(all_domains.size());
  tld_domains.reserve(all_domains.size());
  for (const auto& sv : all_domains) {
    if (memchr(sv.data(), '.', sv.size()) == nullptr) {
      tld_domains.push_back(sv);
    } else {
      regular_domains.push_back(sv);
    }
  }
}

// suffix_last_k_labels returns the last k labels of s, or the whole string if
// s has fewer than k labels. Minimum k is 2.
static std::string_view suffix_last_k_labels(const std::string_view& s, int k) {
  const char* hostname_start = s.data();
  const char* hostname_end = hostname_start + s.size();
  const char* prefix_end = hostname_end;

  for (int i = 0; i < k; i++) {
    const char* label = cut_last_domain_label_naive(hostname_start, prefix_end);
    if (label == hostname_start) {
      return s;
    }

    // -1 to move to previous '.'.
    prefix_end = label - 1;
  }

  // +1 to move after '.', to label beginning.
  const char* label = prefix_end + 1;

  return std::string_view(label, hostname_end - label);
}

// find_popular_suffixes iteratively discovers popular suffixes. Starting at
// depth=2, it groups domains by their last k labels and records suffixes with
// more than D items. If any are found, it increases k and repeats only within
// those groups. Returns a list of unique popular suffixes across all depths.
static std::vector<std::string_view> find_popular_suffixes(
    const std::vector<std::string_view>& domains) {
  std::vector<std::string_view> popular;
  if (domains.empty()) {
    return popular;
  }

  // Initial frontier is the full domain list.
  std::vector<std::string_view> frontier = domains;
  int depth = 2;
  while (true) {
    std::map<std::string_view, std::vector<std::string_view>> buckets;
    for (const auto& s : frontier) {
      std::string_view key = suffix_last_k_labels(s, depth);
      buckets[key].push_back(s);
    }
    std::vector<std::string_view> next_frontier;
    next_frontier.reserve(frontier.size());
    for (auto& kv : buckets) {
      auto& vec = kv.second;
      if (vec.size() > D) {
        popular.push_back(kv.first);
        // Only groups above threshold continue to the next depth.
        next_frontier.insert(next_frontier.end(), vec.begin(), vec.end());
      }
    }
    if (next_frontier.empty()) {
      break;
    }
    frontier.swap(next_frontier);
    depth++;
  }
  // Deduplicate results and provide deterministic ordering.
  std::sort(popular.begin(), popular.end());
  popular.erase(std::unique(popular.begin(), popular.end()), popular.end());
  return popular;
}

// Calibrate seed and bucket count to build preview records without overflow.
// Tries a grid of sizes and seeds:
// - size grows ~5% each step for up to 60 steps
// - tries 20 different seeds per size (incrementing from base_seed)
// Popular suffixes are checked by exact suffix match during bucketing.
// Preview structure for bucket contents during compile-time planning.
struct compile_record_preview_t {
  uint16_t tags[D] = {0};
  std::vector<std::string_view> items;
  uint16_t max_scans = 0;
};

// Helper to compute chained bucket and tag hashes for a domain span.
static inline void build_chained_bucket_and_tag(
    const char* begin, const char* end, uint32_t hash_seed,
    const std::vector<std::string_view>* popular_suffixes_opt,
    uint16_t* out_tag16, uint32_t* out_bucket_hash32, uint16_t* out_max_scans) {
  const char* suffix = cut_last_domain_label_naive(begin, end);
  if (suffix > begin) {
    const char* prev_end = suffix - 1;
    suffix = cut_last_domain_label_naive(begin, prev_end);
  }
  uint64_t h =
      hash64_span_ci(suffix, (size_t)(end - suffix), (uint64_t)hash_seed);

  // Cut popular part from the end if it is present.
  const char* suffix_after_pop = suffix;
  while (suffix_after_pop > begin) {
    if (popular_suffixes_opt) {
      std::string_view cur(suffix_after_pop, (size_t)(end - suffix_after_pop));
      bool is_pop = false;
      for (const auto& sv : *popular_suffixes_opt) {
        if (sv == cur) {
          is_pop = true;
          break;
        }
      }
      if (!is_pop) {
        break;
      }
    } else {
      break;
    }
    const char* label_end = suffix_after_pop - 1;
    const char* label_start = cut_last_domain_label_naive(begin, label_end);
    size_t lab_len = (size_t)(label_end - label_start);
    h = hash64_span_ci(label_start, lab_len, h);
    suffix_after_pop = label_start;
  }

  // Now we can determine which bucket this domain belongs to.
  *out_bucket_hash32 = (uint32_t)h;

  // Go through the parts of the domain remaining in its left part - it
  // corresponds to the hashing and scanning inside a bucket.
  uint64_t hf = h;
  const char* cur = suffix_after_pop;
  uint16_t max_scans = 1;
  while (cur > begin) {
    const char* label_end = cur - 1;
    const char* label_start = cut_last_domain_label_naive(begin, label_end);
    size_t lab_len = (size_t)(label_end - label_start);
    hf = hash64_span_ci(label_start, lab_len, hf);
    cur = label_start;
    max_scans++;
  }
  *out_tag16 = (uint16_t)((hf >> 32) & 0xFFFFu);
  *out_max_scans = max_scans;
}

// Bucketing helper
static bool try_build_records(
    const std::vector<std::string_view>& domains, uint32_t hash_seed,
    uint32_t buckets_num, std::vector<compile_record_preview_t>& out,
    const std::vector<std::string_view>* popular_suffixes_opt) {
  out.clear();
  out.resize(buckets_num);
  const uint64_t M = fastmod::computeM_u32(buckets_num);

  (void)popular_suffixes_opt;

  for (std::string_view sv : domains) {
    if (sv.empty()) {
      continue;
    }
    const char* begin = sv.data();
    const char* end = begin + sv.size();

    uint32_t h_bucket = 0;
    uint16_t h_tag = 0;
    uint16_t max_scans = 0;
    build_chained_bucket_and_tag(begin, end, hash_seed, popular_suffixes_opt,
                                 &h_tag, &h_bucket, &max_scans);
    uint32_t b = fastmod::fastmod_u32(h_bucket, M, buckets_num);
    auto& rec = out[b];
    if (rec.items.size() >= D) {
      return false;
    }
    size_t idx = rec.items.size();
    rec.tags[idx] = h_tag;
    rec.max_scans = std::max(rec.max_scans, max_scans);
    rec.items.push_back(sv);
  }
  return true;
}

// calibration_result_t stores calibration outputs needed to build runtime DB
// memory layout.
struct calibration_result_t {
  // tld_domains contains deduplicated single-label domains.
  std::vector<std::string_view> tld_domains;

  // popular_suffixes contains selected popular suffix strings.
  std::vector<std::string_view> popular_suffixes;

  // preview_buckets holds calibrated per-bucket compile previews.
  std::vector<compile_record_preview_t> preview_buckets;

  // seed is the selected hash seed that avoids bucket overflow.
  uint32_t seed = 0;
};

static inline bool calibrate_and_build_preview(
    const std::vector<std::string_view>& domains_views,
    const std::vector<std::string_view>& popular_suffixes,
    calibration_result_t& res) {
  // Start with minimum sane number of buckets.
  size_t buckets_num = domains_views.size() / D + 1;

  // Some random value.
  uint32_t seed = 0xA17F2344u;

  for (int grow = 0; grow < CALIB_GROW_STEPS; grow++) {
    for (int t = 0; t < CALIB_SEED_TRIES; t++) {
      seed++;
      debugf("[calib] try buckets=%zu seed=0x%08x\n", buckets_num, seed);

      std::vector<compile_record_preview_t> preview_buckets;
      bool ok = try_build_records(domains_views, seed, (uint32_t)buckets_num,
                                  preview_buckets, &popular_suffixes);
      if (ok) {
        debugf("[calib]   SUCCESS buckets=%zu seed=0x%08x\n", buckets_num,
               seed);
        res.seed = seed;
        res.popular_suffixes = popular_suffixes;
        res.preview_buckets = std::move(preview_buckets);
        return true;
      } else {
        debugf("[calib]   overflow -> retry next seed\n");
      }
    }
    // Increase buckets by ~5%, at least +1
    size_t buckets_num2 = (buckets_num * CALIB_GROW_NUM) / CALIB_GROW_DEN;
    size_t new_buckets = std::max(buckets_num2, buckets_num + 1);
    debugf("[calib] grow buckets: %zu -> %zu\n", buckets_num, new_buckets);
    buckets_num = new_buckets;
  }
  return false;
}

// Build the runtime DB into db_place from calibration preview.
static hm_error_t build_db_from_preview(char* db_place, size_t db_place_size,
                                        hm_domain_database_t** db_ptr,
                                        const calibration_result_t& calib) {
  char* p = align(db_place, 64);
  if ((size_t)(p - db_place) > db_place_size) {
    return HM_ERROR_SMALL_PLACE;
  }

  size_t place_left = db_place_size - (size_t)(p - db_place);
  if (place_left < round_up64(sizeof(hm_domain_database_t))) {
    return HM_ERROR_SMALL_PLACE;
  }

  memset(p, 0, round_up64(sizeof(hm_domain_database_t)));
  hm_domain_database_t* db = (hm_domain_database_t*)p;
  p += round_up64(sizeof(hm_domain_database_t));

  size_t buckets_num = calib.preview_buckets.size();

  // Calculate sizes of remaining components: tld table, popular table, main
  // buckets table, and domains blob.
  size_t tld_records = (calib.tld_domains.size() + D - 1) / D;
  size_t tld_bytes = tld_records * sizeof(domains_table_record_t);
  size_t table_bytes = buckets_num * sizeof(domains_table_record_t);
  size_t popular_records = (calib.popular_suffixes.size() + D - 1) / D;
  size_t popular_bytes = popular_records * sizeof(domains_table_record_t);
  size_t blob_bytes = 0;
  for (const auto& sv : calib.tld_domains) {
    blob_bytes += round_up16(sv.size() + 1);
  }
  for (const auto& sv : calib.popular_suffixes) {
    blob_bytes += round_up16(sv.size() + 1);
  }
  for (const auto& bucket : calib.preview_buckets) {
    for (const auto& domain : bucket.items) {
      // +1 for 0x00 byte in the end.
      blob_bytes += round_up16(domain.size() + 1);
    }
  }
  // Add space for memcmp inside scan_tags and general SIMD tail.
  blob_bytes += 256;

  size_t total_bytes_after_header =
      tld_bytes + popular_bytes + table_bytes + blob_bytes;

  place_left = db_place_size - (size_t)(p - db_place);
  if (place_left < total_bytes_after_header) {
    return HM_ERROR_SMALL_PLACE;
  }

  memset(p, 0, total_bytes_after_header);

  // Fill DB
  *db_ptr = db;
  db->buckets = buckets_num;
  db->fastmod_M = fastmod::computeM_u32(buckets_num);
  db->hash_seed = calib.seed;

  // TLD table goes first after the header.
  db->tld_table = (domains_table_record_t*)p;
  db->tld_records = (uint32_t)tld_records;
  db->tld_count = (uint32_t)calib.tld_domains.size();
  p += tld_bytes;

  // Then popular table.
  db->popular_table = (domains_table_record_t*)p;
  db->popular_records = (uint32_t)popular_records;
  db->popular_count = (uint32_t)calib.popular_suffixes.size();
  p += popular_bytes;

  // Then the main buckets table
  db->domains_table = (domains_table_record_t*)p;
  p += table_bytes;

  db->domains_blob = (char*)p;
  char* blob = db->domains_blob;
  size_t cur_blob = 0;

  // Build TLD table.
  for (uint32_t r = 0; r < db->tld_records; r++) {
    domains_table_record_t& dst_rec = db->tld_table[r];
    dst_rec.used_slots = 0;
    dst_rec.max_scans = 0;
    dst_rec.domains_blob_offset = (uint32_t)cur_blob;
    dst_rec.domains_blob = blob + cur_blob;
    size_t base_off = cur_blob;
    for (uint32_t i = 0; i < D; i++) {
      size_t idx = (size_t)r * D + i;
      if (idx >= calib.tld_domains.size()) {
        break;
      }
      std::string_view sv = calib.tld_domains[idx];
      uint64_t h =
          hash64_span_ci(sv.data(), sv.size(), (uint64_t)db->hash_seed);
      uint16_t tag = (uint16_t)((h >> 32) & 0xFFFFu);
      size_t off_units = (cur_blob - base_off) / D;
      assert(off_units <= 255);
      dst_rec.domains_offsets[dst_rec.used_slots] = (uint8_t)off_units;
      dst_rec.domains_hashes[dst_rec.used_slots] = tag;
      memcpy(blob + cur_blob, sv.data(), sv.size());
      blob[cur_blob + sv.size()] = '\0';
      cur_blob += round_up16(sv.size() + 1);
      dst_rec.used_slots++;
    }
    if (dst_rec.used_slots > 0) {
      dst_rec.max_scans = 1;
    }
  }

  // Build popular table: lay out suffixes and fill records.
  for (uint32_t r = 0; r < db->popular_records; r++) {
    domains_table_record_t& dst_rec = db->popular_table[r];
    dst_rec.used_slots = 0;
    dst_rec.domains_blob_offset = (uint32_t)cur_blob;
    dst_rec.domains_blob = blob + cur_blob;
    size_t base_off = cur_blob;
    for (uint32_t i = 0; i < D; i++) {
      size_t idx = (size_t)r * D + i;
      if (idx >= calib.popular_suffixes.size()) {
        break;
      }
      std::string_view sv = calib.popular_suffixes[idx];
      // Compute tag for this suffix in the same chained order as
      // hm_domain_find: start from its last two labels, then chain labels to
      // the left.
      const char* begin = sv.data();
      const char* end = begin + sv.size();
      const char* suffix2 = cut_last_domain_label_naive(begin, end);
      if (suffix2 > begin) {
        const char* prev_end = suffix2 - 1;
        suffix2 = cut_last_domain_label_naive(begin, prev_end);
      }
      uint64_t h = hash64_span_ci(suffix2, (size_t)(end - suffix2),
                                  (uint64_t)db->hash_seed);
      const char* cur2 = suffix2;
      while (cur2 > begin) {
        const char* label_end = cur2 - 1;
        const char* label_start = cut_last_domain_label_naive(begin, label_end);
        size_t lab_len = (size_t)(label_end - label_start);
        h = hash64_span_ci(label_start, lab_len, h);
        cur2 = label_start;
      }
      uint16_t tag = (uint16_t)((h >> 32) & 0xFFFFu);
      size_t off_units = (cur_blob - base_off) / D;
      assert(off_units <= 255);
      dst_rec.domains_offsets[dst_rec.used_slots] = (uint8_t)off_units;
      dst_rec.domains_hashes[dst_rec.used_slots] = tag;
      memcpy(blob + cur_blob, sv.data(), sv.size());
      blob[cur_blob + sv.size()] = '\0';
      cur_blob += round_up16(sv.size() + 1);
      dst_rec.used_slots++;
    }
  }
  for (uint32_t b = 0; b < buckets_num; b++) {
    domains_table_record_t& dst_rec = db->domains_table[b];
    const compile_record_preview_t& src_rec = calib.preview_buckets[b];

    dst_rec.used_slots = (uint16_t)src_rec.items.size();
    dst_rec.max_scans = src_rec.max_scans;
    dst_rec.domains_blob_offset = (uint32_t)cur_blob;
    dst_rec.domains_blob = blob + cur_blob;

    size_t base_off = cur_blob;
    for (size_t i = 0; i < src_rec.items.size(); i++) {
      size_t off_units = (cur_blob - base_off) / D;
      assert(off_units <= 255);
      dst_rec.domains_offsets[i] = (uint8_t)off_units;
      dst_rec.domains_hashes[i] = src_rec.tags[i];

      std::string_view domain = src_rec.items[i];
      memcpy(blob + cur_blob, domain.data(), domain.size());
      blob[cur_blob + domain.size()] = '\0';
      cur_blob += round_up16(domain.size() + 1);
    }
  }
  db->domains_blob_size = blob_bytes;

  // Sanity check: verify we came to the same value for domain blob size as
  // above.
  assert(cur_blob + 256 <= blob_bytes);

  return HM_SUCCESS;
}

extern "C" size_t HM_CDECL hm_domain_db_place_size(const char** domains,
                                                   unsigned int elements) {
  if (elements == 0 || domains == nullptr) {
    debugf("[size] invalid inputs: elements=%u ptr=%p\n", elements,
           (void*)domains);
    return 0;
  }
  std::list<std::string> domains_lower;
  std::vector<std::string_view> domains_views;
  if (!preprocess_domains_views(domains, elements, domains_lower,
                                domains_views)) {
    debugf("[size] preprocess failed\n");
    return 0;
  }
  if (domains_views.empty()) {
    debugf("[size] no domains after preprocess\n");
    return 0;
  }

  std::vector<std::string_view> regular_domains;
  std::vector<std::string_view> tld_domains;
  split_regular_and_tld_domains(domains_views, regular_domains, tld_domains);

  // Start with minimum sane number of buckets for regular domains.
  size_t buckets_num = regular_domains.size() / D + 1;
  if (regular_domains.empty()) {
    buckets_num = 1;
  } else {
    for (int grow = 0; grow < CALIB_GROW_STEPS; grow++) {
      // Increase buckets by ~5%, at least +1
      size_t buckets_num2 = (buckets_num * CALIB_GROW_NUM) / CALIB_GROW_DEN;
      buckets_num = std::max(buckets_num2, buckets_num + 1);
    }
  }

  // Estimate popular suffixes to size popular table and blob head.
  auto popular_suffixes = find_popular_suffixes(regular_domains);

  // Find blob size. Assume every string is D-aligned in its bucket region.
  size_t blob_bytes = 0;
  for (auto sv : tld_domains) {
    size_t s_len = sv.size() + 1;  // 0x00 in the end
    blob_bytes += round_up16(s_len);
  }
  for (auto sv : popular_suffixes) {
    size_t s_len = sv.size() + 1;  // 0x00 in the end
    blob_bytes += round_up16(s_len);
  }
  for (auto sv : regular_domains) {
    size_t domain_len = sv.size() + 1;  // 0x00 byte in the end.
    blob_bytes += round_up16(domain_len);
  }
  // Add space for memcmp inside scan_tags and general SIMD tail.
  blob_bytes += 256;

  size_t tld_records = (tld_domains.size() + D - 1) / D;
  size_t tld_bytes = tld_records * sizeof(domains_table_record_t);
  size_t popular_records = (popular_suffixes.size() + D - 1) / D;
  size_t popular_bytes = popular_records * sizeof(domains_table_record_t);
  size_t table_bytes = buckets_num * sizeof(domains_table_record_t);

  return round_up64(sizeof(hm_domain_database_t)) + tld_bytes + popular_bytes +
         table_bytes + blob_bytes + 2 * ALIGN_HEADROOM;
}

extern "C" hm_error_t HM_CDECL hm_domain_compile(char* db_place,
                                                 size_t db_place_size,
                                                 hm_domain_database_t** db_ptr,
                                                 const char** domains,
                                                 unsigned int elements) {
  if (elements <= 0) {
    debugf("[compile] invalid elements=%u\n", elements);
    return HM_ERROR_NO_MASKS;
  }

  // Place to allocate lower-cased domains if needed.
  std::list<std::string> domains_lower;

  // Here we accumulate the domains going into DB.
  std::vector<std::string_view> domains_views;

  // Preprocess inputs to cleaned views (lowercased, trimmed, pruned)
  if (!preprocess_domains_views(domains, elements, domains_lower,
                                domains_views)) {
    debugf("[compile] preprocess failed\n");
    return HM_ERROR_BAD_VALUE;
  }

  std::vector<std::string_view> regular_domains;
  std::vector<std::string_view> tld_domains;
  split_regular_and_tld_domains(domains_views, regular_domains, tld_domains);

  debugf("[compile] inputs=%u after-preprocess=%zu\n", elements,
         domains_views.size());
  debugf("[compile] regular=%zu tld=%zu\n", regular_domains.size(),
         tld_domains.size());
  for (size_t i = 0; i < domains_views.size(); i++) {
    const auto& sv = domains_views[i];
    debugf("[compile]   domain[%zu]=%.*s\n", i, (int)sv.size(), sv.data());
  }

  // Find popular suffixes and check that their number is not too high.
  auto popular_suffixes = find_popular_suffixes(regular_domains);
  if (popular_suffixes.size() > MAX_POPULAR_SUFFIXES) {
    return HM_ERROR_TOO_MANY_POPULAR_DOMAINS;
  }

  debugf("[compile] popular candidates=%zu\n", popular_suffixes.size());
  for (size_t i = 0; i < popular_suffixes.size(); i++) {
    const auto& sv = popular_suffixes[i];
    debugf("[compile]   popular[%zu]=%.*s\n", i, (int)sv.size(), sv.data());
  }

  // Now calibrate the table to fit all the domains.
  calibration_result_t calib;
  if (!calibrate_and_build_preview(regular_domains, popular_suffixes, calib)) {
    return HM_ERROR_FAILED_TO_CALIBRATE;
  }
  calib.tld_domains = tld_domains;

  debugf("[compile] calibrated: buckets=%zu seed=0x%08x popular_count=%zu\n",
         calib.preview_buckets.size(), calib.seed,
         calib.popular_suffixes.size());
  for (size_t b = 0; b < calib.preview_buckets.size(); b++) {
    const auto& rec = calib.preview_buckets[b];
    if (!rec.items.empty()) {
      debugf("[compile]   bucket %zu: used=%zu max_scans=%u\n", b,
             rec.items.size(), rec.max_scans);
      for (size_t i = 0; i < rec.items.size(); i++) {
        const auto& it = rec.items[i];
        debugf("[compile]     tag[%zu]=0x%04x domain=%.*s\n", i, rec.tags[i],
               (int)it.size(), it.data());
      }
    }
  }

  // Build DB from preview and return.
  return build_db_from_preview(db_place, db_place_size, db_ptr, calib);
}

// Lightweight getters for Go summary without parsing serialized buffers.
extern "C" uint32_t HM_CDECL hm_domain_buckets(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return db->buckets;
}

extern "C" uint32_t HM_CDECL
hm_domain_popular_count(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return (uint32_t)db->popular_count;
}

extern "C" uint32_t HM_CDECL
hm_domain_tld_count(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return (uint32_t)db->tld_count;
}

extern "C" uint32_t HM_CDECL
hm_domain_used_total(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  uint32_t total = db->tld_count;
  for (uint32_t b = 0; b < db->buckets; b++) {
    total += db->domains_table[b].used_slots;
  }
  return total;
}

extern "C" uint32_t HM_CDECL
hm_domain_hash_seed(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return db->hash_seed;
}

extern "C" size_t HM_CDECL hm_domain_header_bytes() {
  return SERIALIZED_HEADER_BYTES;
}

extern "C" size_t HM_CDECL
hm_domain_table_bytes(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return (size_t)db->buckets * sizeof(domains_table_record_t);
}

extern "C" size_t HM_CDECL hm_domain_tld_bytes(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return (size_t)db->tld_records * sizeof(domains_table_record_t);
}

extern "C" size_t HM_CDECL
hm_domain_popular_bytes(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return (size_t)db->popular_records * sizeof(domains_table_record_t);
}

extern "C" size_t HM_CDECL
hm_domain_blob_bytes(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return db->domains_blob_size;
}

// Serialized form (v2) is:
// [4-byte magic] [64-byte header] [tld records] [popular records]
// [bucket records] [domains blob].
//
// Pointer fields in records are canonicalized to zero while writing to keep
// serialized output deterministic across runs and process layouts.
extern "C" size_t HM_CDECL
hm_domain_serialized_size(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  size_t tld_bytes = (size_t)db->tld_records * sizeof(domains_table_record_t);
  size_t popular_bytes =
      (size_t)db->popular_records * sizeof(domains_table_record_t);
  size_t table_bytes = (size_t)db->buckets * sizeof(domains_table_record_t);
  return 4 + SERIALIZED_HEADER_BYTES + tld_bytes + popular_bytes + table_bytes +
         db->domains_blob_size;
}

extern "C" hm_error_t HM_CDECL hm_domain_serialize(
    char* buffer, size_t buffer_size, const hm_domain_database_t* db) {
  if (!db || !buffer) {
    return HM_ERROR_BAD_VALUE;
  }
  size_t need = hm_domain_serialized_size(db);
  if (buffer_size < need) {
    return HM_ERROR_SMALL_PLACE;
  }

  size_t tld_bytes = (size_t)db->tld_records * sizeof(domains_table_record_t);
  size_t popular_bytes =
      (size_t)db->popular_records * sizeof(domains_table_record_t);
  size_t table_bytes = (size_t)db->buckets * sizeof(domains_table_record_t);
  size_t payload_bytes = SERIALIZED_HEADER_BYTES + tld_bytes + popular_bytes +
                         table_bytes + db->domains_blob_size;
  if (need != 4 + payload_bytes) {
    return HM_ERROR_BAD_VALUE;
  }
  if ((db->tld_records > 0 && !db->tld_table) ||
      (db->popular_records > 0 && !db->popular_table) ||
      (db->buckets > 0 && !db->domains_table) || !db->domains_blob) {
    return HM_ERROR_BAD_VALUE;
  }

  write_u32(buffer, STATIC_DOMAIN_SET_MAGIC_V2);
  char* hdr = buffer + 4;
  memset(hdr, 0, SERIALIZED_HEADER_BYTES);
  write_u64(hdr + 0, db->fastmod_M);
  write_u32(hdr + 8, db->buckets);
  write_u32(hdr + 12, db->hash_seed);
  write_u32(hdr + 16, db->popular_records);
  write_u32(hdr + 20, db->popular_count);
  write_u32(hdr + 24, db->tld_records);
  write_u32(hdr + 28, db->tld_count);
  write_u64(hdr + 32, (uint64_t)db->domains_blob_size);

  char* p = hdr + SERIALIZED_HEADER_BYTES;
  for (uint32_t i = 0; i < db->tld_records; i++) {
    domains_table_record_t rec = db->tld_table[i];
    rec.domains_blob = nullptr;
    memcpy(p, &rec, sizeof(rec));
    p += sizeof(rec);
  }
  for (uint32_t i = 0; i < db->popular_records; i++) {
    domains_table_record_t rec = db->popular_table[i];
    rec.domains_blob = nullptr;
    memcpy(p, &rec, sizeof(rec));
    p += sizeof(rec);
  }
  for (uint32_t i = 0; i < db->buckets; i++) {
    domains_table_record_t rec = db->domains_table[i];
    rec.domains_blob = nullptr;
    memcpy(p, &rec, sizeof(rec));
    p += sizeof(rec);
  }
  memcpy(p, db->domains_blob, db->domains_blob_size);
  return HM_SUCCESS;
}

extern "C" hm_error_t HM_CDECL hm_domain_db_place_size_from_serialized(
    size_t* db_place_size, const char* buffer, size_t buffer_size) {
  if (!db_place_size) {
    return HM_ERROR_BAD_VALUE;
  }
  if (buffer_size < 4 + SERIALIZED_HEADER_BYTES) {
    return HM_ERROR_SMALL_PLACE;
  }
  serialized_meta_t meta;
  if (!parse_serialized_meta(buffer, buffer_size, &meta)) {
    return HM_ERROR_BAD_VALUE;
  }
  if (meta.blob_bytes % 16 != 0 || meta.blob_bytes < 256) {
    return HM_ERROR_BAD_VALUE;
  }
  if (meta.buckets == 0) {
    return HM_ERROR_BAD_VALUE;
  }
  if (meta.blob_bytes > (uint64_t)SIZE_MAX) {
    return HM_ERROR_BAD_VALUE;
  }
  if (meta.buckets > (SIZE_MAX / sizeof(domains_table_record_t))) {
    return HM_ERROR_BAD_VALUE;
  }
  if (meta.popular_records > (SIZE_MAX / sizeof(domains_table_record_t))) {
    return HM_ERROR_BAD_VALUE;
  }
  if (meta.tld_records > (SIZE_MAX / sizeof(domains_table_record_t))) {
    return HM_ERROR_BAD_VALUE;
  }
  if ((uint64_t)meta.popular_count > (uint64_t)meta.popular_records * D) {
    return HM_ERROR_BAD_VALUE;
  }
  if ((uint64_t)meta.tld_count > (uint64_t)meta.tld_records * D) {
    return HM_ERROR_BAD_VALUE;
  }

  size_t table_bytes = 0;
  size_t popular_bytes = 0;
  size_t tld_bytes = 0;
  if (mul_size_overflow((size_t)meta.buckets, sizeof(domains_table_record_t),
                        &table_bytes) ||
      mul_size_overflow((size_t)meta.popular_records,
                        sizeof(domains_table_record_t), &popular_bytes) ||
      mul_size_overflow((size_t)meta.tld_records,
                        sizeof(domains_table_record_t), &tld_bytes)) {
    return HM_ERROR_BAD_VALUE;
  }
  size_t blob_bytes = (size_t)meta.blob_bytes;
  size_t total_bytes_after_header = 0;
  if (add_size_overflow(tld_bytes, popular_bytes, &total_bytes_after_header) ||
      add_size_overflow(total_bytes_after_header, table_bytes,
                        &total_bytes_after_header) ||
      add_size_overflow(total_bytes_after_header, blob_bytes,
                        &total_bytes_after_header)) {
    return HM_ERROR_BAD_VALUE;
  }

  size_t place_left = buffer_size - (4 + SERIALIZED_HEADER_BYTES);
  if (place_left < total_bytes_after_header) {
    return HM_ERROR_SMALL_PLACE;
  }

  size_t out = 0;
  if (add_size_overflow(round_up64(sizeof(hm_domain_database_t)),
                        total_bytes_after_header, &out) ||
      add_size_overflow(out, (size_t)2 * ALIGN_HEADROOM, &out)) {
    return HM_ERROR_BAD_VALUE;
  }
  *db_place_size = out;
  return HM_SUCCESS;
}

extern "C" hm_error_t HM_CDECL hm_domain_deserialize(
    char* db_place, size_t db_place_size, hm_domain_database_t** db_ptr,
    const char* buffer, size_t buffer_size) {
  if (!db_place || !db_ptr || !buffer) {
    return HM_ERROR_BAD_VALUE;
  }
  size_t need = 0;
  hm_error_t err =
      hm_domain_db_place_size_from_serialized(&need, buffer, buffer_size);
  if (err != HM_SUCCESS) {
    return err;
  }
  if (db_place_size < need) {
    return HM_ERROR_SMALL_PLACE;
  }

  serialized_meta_t meta;
  if (!parse_serialized_meta(buffer, buffer_size, &meta)) {
    return HM_ERROR_BAD_VALUE;
  }

  const char* src = buffer + 4 + SERIALIZED_HEADER_BYTES;
  size_t table_bytes = 0;
  size_t popular_bytes = 0;
  size_t tld_bytes = 0;
  if (mul_size_overflow((size_t)meta.buckets, sizeof(domains_table_record_t),
                        &table_bytes) ||
      mul_size_overflow((size_t)meta.popular_records,
                        sizeof(domains_table_record_t), &popular_bytes) ||
      mul_size_overflow((size_t)meta.tld_records,
                        sizeof(domains_table_record_t), &tld_bytes)) {
    return HM_ERROR_BAD_VALUE;
  }
  if (meta.blob_bytes > (uint64_t)SIZE_MAX) {
    return HM_ERROR_BAD_VALUE;
  }
  size_t blob_bytes = (size_t)meta.blob_bytes;
  size_t serialized_after_header = 0;
  if (add_size_overflow(tld_bytes, popular_bytes, &serialized_after_header) ||
      add_size_overflow(serialized_after_header, table_bytes,
                        &serialized_after_header) ||
      add_size_overflow(serialized_after_header, blob_bytes,
                        &serialized_after_header)) {
    return HM_ERROR_BAD_VALUE;
  }
  size_t hdr_off = (size_t)(src - buffer);
  if (hdr_off > buffer_size ||
      serialized_after_header > buffer_size - hdr_off) {
    return HM_ERROR_SMALL_PLACE;
  }

  // Align destination base
  char* base = align(db_place, 64);
  if ((size_t)(base - db_place) > db_place_size) {
    return HM_ERROR_SMALL_PLACE;
  }
  char* dst = base;
  memset(dst, 0, round_up64(sizeof(hm_domain_database_t)));
  hm_domain_database_t* db = (hm_domain_database_t*)dst;
  *db_ptr = db;
  db->buckets = meta.buckets;
  db->fastmod_M = meta.fastmod_M;
  db->hash_seed = meta.hash_seed;
  db->popular_count = meta.popular_count;
  db->tld_count = meta.tld_count;
  dst += round_up64(sizeof(hm_domain_database_t));

  if (meta.is_v2) {
    db->tld_records = meta.tld_records;
    if (tld_bytes > 0) {
      db->tld_table = (domains_table_record_t*)dst;
      memcpy(db->tld_table, src, tld_bytes);
      src += tld_bytes;
      dst += tld_bytes;
    } else {
      db->tld_table = nullptr;
    }

    db->popular_records = meta.popular_records;
    if (popular_bytes > 0) {
      db->popular_table = (domains_table_record_t*)dst;
      memcpy(db->popular_table, src, popular_bytes);
      src += popular_bytes;
      dst += popular_bytes;
    } else {
      db->popular_table = nullptr;
    }
  } else {
    // v1 serialized layout has no TLD table and stores popular first.
    db->tld_records = 0;
    db->tld_table = nullptr;

    db->popular_records = meta.popular_records;
    if (popular_bytes > 0) {
      db->popular_table = (domains_table_record_t*)dst;
      memcpy(db->popular_table, src, popular_bytes);
      src += popular_bytes;
      dst += popular_bytes;
    } else {
      db->popular_table = nullptr;
    }
  }

  if (table_bytes > 0) {
    db->domains_table = (domains_table_record_t*)dst;
    memcpy(db->domains_table, src, table_bytes);
    src += table_bytes;
    dst += table_bytes;
  } else {
    db->domains_table = nullptr;
  }

  db->domains_blob = (char*)dst;
  db->domains_blob_size = blob_bytes;
  memcpy(db->domains_blob, src, blob_bytes);

  auto rebuild_table = [&](domains_table_record_t* recs,
                           uint32_t rec_count) -> bool {
    if (!recs || rec_count == 0) {
      return true;
    }
    for (uint32_t r = 0; r < rec_count; r++) {
      domains_table_record_t* rec = &recs[r];
      if (rec->used_slots > D) {
        return false;
      }
      size_t base_off = rec->domains_blob_offset;
      if (base_off > db->domains_blob_size) {
        return false;
      }
      rec->domains_blob = db->domains_blob + base_off;
      for (uint32_t i = 0; i < rec->used_slots; i++) {
        size_t off_units = (size_t)rec->domains_offsets[i];
        size_t pos = base_off + off_units * D;
        if (pos + MAX_DOMAIN_LEN >= db->domains_blob_size) {
          return false;
        }
      }
    }
    return true;
  };

  if (!rebuild_table(db->domains_table, db->buckets)) {
    return HM_ERROR_BAD_VALUE;
  }
  if (!rebuild_table(db->popular_table, db->popular_records)) {
    return HM_ERROR_BAD_VALUE;
  }
  if (!rebuild_table(db->tld_table, db->tld_records)) {
    return HM_ERROR_BAD_VALUE;
  }

  return HM_SUCCESS;
}

extern "C" uint64_t HM_CDECL hm_domain_hash64_span_ci(const char* s, size_t len,
                                                      uint64_t seed) {
  return hash64_span_ci(s, len, seed);
}

extern "C" uint16_t HM_CDECL hm_domain_hash(const char* s, size_t len,
                                            uint64_t seed) {
  return (uint16_t)(hash64_span_ci(s, len, seed) & 0xFFFF);
}

extern "C" size_t HM_CDECL hm_cut_last_domain_label_offset(const char* domain,
                                                           size_t len) {
  const char* start = domain;
  const char* end = domain + len;
  const char* p = cut_last_domain_label_naive(start, end);
  return (size_t)(p - start);
}
