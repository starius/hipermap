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

#define STATIC_DOMAIN_SET_MAGIC 0x53444D48u

static_assert(sizeof(domains_table_record_t) == 64, "Struct must be 64 bytes");

static inline char* align(char* addr, size_t alignment) {
  return (char*)(((uintptr_t)(addr) & ~(uintptr_t)(alignment - 1)) + alignment);
}

static inline size_t round_up16(size_t x) { return (x + 15) & ~15; }

static inline size_t round_up64(size_t x) { return (x + 63) & ~63; }

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

struct calibration_result_t {
  std::vector<std::string_view> popular_suffixes;
  std::vector<compile_record_preview_t> preview_buckets;
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

  // Calculate sizes of remaining components: buckets table, popular_table, and
  // the domains blob (popular suffixes first).
  size_t table_bytes = buckets_num * sizeof(domains_table_record_t);
  size_t popular_records = (calib.popular_suffixes.size() + D - 1) / D;
  size_t popular_bytes = popular_records * sizeof(domains_table_record_t);
  size_t blob_bytes = 0;
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

  size_t total_bytes_after_header = table_bytes + popular_bytes + blob_bytes;

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

  // Popular table goes first after the header (aligned)
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

  // Build popular table first: lay out suffixes and fill records
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
  const size_t kAlignHeadroom = 64;
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

  // Start with minimum sane number of buckets.
  size_t buckets_num = domains_views.size() / D + 1;

  for (int grow = 0; grow < CALIB_GROW_STEPS; grow++) {
    // Increase buckets by ~5%, at least +1
    size_t buckets_num2 = (buckets_num * CALIB_GROW_NUM) / CALIB_GROW_DEN;
    buckets_num = std::max(buckets_num2, buckets_num + 1);
  }

  // Estimate popular suffixes to size popular table and blob head.
  auto popular_suffixes = find_popular_suffixes(domains_views);

  // Find blob size. Assume every string is D-aligned in its bucket region.
  size_t blob_bytes = 0;
  for (auto sv : popular_suffixes) {
    size_t s_len = sv.size() + 1;  // 0x00 in the end
    blob_bytes += round_up16(s_len);
  }
  for (auto sv : domains_views) {
    size_t domain_len = sv.size() + 1;  // 0x00 byte in the end.
    blob_bytes += round_up16(domain_len);
  }
  // Add space for memcmp inside scan_tags and general SIMD tail.
  blob_bytes += 256;

  size_t popular_records = (popular_suffixes.size() + D - 1) / D;
  size_t popular_bytes = popular_records * sizeof(domains_table_record_t);
  size_t table_bytes = buckets_num * sizeof(domains_table_record_t);

  return sizeof(hm_domain_database_t) + table_bytes + popular_bytes +
         blob_bytes + 2 * kAlignHeadroom;
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

  // Reject top-level domains (no dot) since the fast path cuts by labels
  // starting from the last two components.
  for (const auto& sv : domains_views) {
    if (memchr(sv.data(), '.', sv.size()) == nullptr) {
      return HM_ERROR_TOP_LEVEL_DOMAIN;
    }
  }

  debugf("[compile] inputs=%u after-preprocess=%zu\n", elements,
         domains_views.size());
  for (size_t i = 0; i < domains_views.size(); i++) {
    const auto& sv = domains_views[i];
    debugf("[compile]   domain[%zu]=%.*s\n", i, (int)sv.size(), sv.data());
  }

  // Find popular suffixes and check that their number is not too high.
  auto popular_suffixes = find_popular_suffixes(domains_views);
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
  if (!calibrate_and_build_preview(domains_views, popular_suffixes, calib)) {
    return HM_ERROR_BAD_VALUE;
  }

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
hm_domain_used_total(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  uint32_t total = 0;
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
  return round_up64(sizeof(hm_domain_database_t));
}

extern "C" size_t HM_CDECL
hm_domain_table_bytes(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  return (size_t)db->buckets * sizeof(domains_table_record_t);
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

// Serialized form is the same as db_place starting from db header and
// up to the end of domains_blob. Prefixed with the magic 4 bytes.

extern "C" size_t HM_CDECL
hm_domain_serialized_size(const hm_domain_database_t* db) {
  if (!db) {
    return 0;
  }
  size_t size = 0;

  // Magic.
  size += 4;

  const char* begin = (const char*)(db);
  const char* end = db->domains_blob + db->domains_blob_size;

  size += (end - begin);

  return size;
}

extern "C" hm_error_t HM_CDECL hm_domain_serialize(
    char* buffer, size_t buffer_size, const hm_domain_database_t* db) {
  if (!db) {
    return HM_ERROR_BAD_VALUE;
  }
  size_t need = hm_domain_serialized_size(db);
  if (buffer_size < need) {
    return HM_ERROR_SMALL_PLACE;
  }

  uint32_t* magic = (uint32_t*)buffer;
  *magic = STATIC_DOMAIN_SET_MAGIC;

  buffer += sizeof(uint32_t);
  // Copy the serialized payload excluding the 4-byte magic header we already
  // wrote.
  memcpy(buffer, db, need - sizeof(uint32_t));

  return HM_SUCCESS;
}

extern "C" hm_error_t HM_CDECL hm_domain_db_place_size_from_serialized(
    size_t* db_place_size, const char* buffer, size_t buffer_size) {
  if (!db_place_size) {
    return HM_ERROR_BAD_VALUE;
  }
  if (buffer_size < 4 + 64) {
    return HM_ERROR_SMALL_PLACE;
  }
  const char* p = buffer;
  uint32_t magic;
  memcpy(&magic, p, sizeof(magic));
  if (magic != STATIC_DOMAIN_SET_MAGIC) {
    return HM_ERROR_BAD_VALUE;
  }
  p += 4;
  // Header is hm_domain_database_t padded to 64 bytes in serialized form.
  hm_domain_database_t hdr = {};
  memcpy(&hdr, p, sizeof(hdr) <= 64 ? sizeof(hdr) : 64);
  p += 64;
  size_t buckets = hdr.buckets;
  size_t popular_records = hdr.popular_records;
  size_t blob_bytes = hdr.domains_blob_size;
  if (blob_bytes % 16 != 0) {
    return HM_ERROR_BAD_VALUE;
  }
  if (blob_bytes < 256) {
    return HM_ERROR_BAD_VALUE;
  }
  // Basic sanity checks
  if (buckets > (SIZE_MAX / sizeof(domains_table_record_t))) {
    return HM_ERROR_BAD_VALUE;
  }
  size_t table_bytes = buckets * sizeof(domains_table_record_t);
  size_t popular_bytes = popular_records * sizeof(domains_table_record_t);

  size_t total_bytes_after_header = table_bytes + popular_bytes + blob_bytes;

  size_t place_left = buffer_size - (size_t)(p - buffer);
  if (place_left < total_bytes_after_header) {
    return HM_ERROR_SMALL_PLACE;
  }

  // Compute required db_place size with extra alignment headroom, similar to
  // hm_domain_db_place_size: callers may pass an arbitrary base pointer which
  // we realign inside hm_domain_deserialize. Provide headroom to keep the
  // aligned destination within the allocated buffer and to tolerate minor
  // overreads by SIMD.
  *db_place_size = round_up64(sizeof(hm_domain_database_t)) +
                   total_bytes_after_header + 2 * ALIGN_HEADROOM;

  return HM_SUCCESS;
}

extern "C" hm_error_t HM_CDECL hm_domain_deserialize(
    char* db_place, size_t db_place_size, hm_domain_database_t** db_ptr,
    const char* buffer, size_t buffer_size) {
  if (!db_place || !db_ptr) {
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

  const char* src = buffer;
  if (buffer_size < 4 + 64) {
    return HM_ERROR_SMALL_PLACE;
  }
  uint32_t magic;
  memcpy(&magic, src, sizeof(magic));
  if (magic != STATIC_DOMAIN_SET_MAGIC) {
    return HM_ERROR_BAD_VALUE;
  }
  src += 4;
  hm_domain_database_t hdr = {};
  memcpy(&hdr, src, sizeof(hdr) <= 64 ? sizeof(hdr) : 64);
  src += round_up64(sizeof(hm_domain_database_t));

  uint32_t buckets = hdr.buckets;
  uint32_t popular_records = hdr.popular_records;
  uint32_t blob_bytes = hdr.domains_blob_size;
  uint64_t fastmod_M = hdr.fastmod_M;
  uint32_t seed = hdr.hash_seed;

  size_t table_bytes = (size_t)buckets * sizeof(domains_table_record_t);
  size_t popular_bytes =
      (size_t)popular_records * sizeof(domains_table_record_t);
  // Bounds check source buffer
  if ((size_t)(src - buffer) + table_bytes + popular_bytes + blob_bytes >
      buffer_size) {
    return HM_ERROR_SMALL_PLACE;
  }

  // Align destination base
  char* base = align(db_place, 64);
  if ((size_t)(base - db_place) > db_place_size) {
    return HM_ERROR_SMALL_PLACE;
  }
  char* dst = base;
  hm_domain_database_t* db = (hm_domain_database_t*)dst;
  *db_ptr = db;
  db->buckets = buckets;
  db->fastmod_M = fastmod_M;
  db->hash_seed = seed;
  db->popular_count = hdr.popular_count;
  dst += round_up64(sizeof(hm_domain_database_t));

  // Popular table first in the serialized buffer after header
  db->popular_table = (domains_table_record_t*)dst;
  db->popular_records = popular_records;
  memcpy(db->popular_table, src, popular_bytes);
  src += popular_bytes;
  dst += popular_bytes;

  // Then domains table
  db->domains_table = (domains_table_record_t*)dst;
  memcpy(db->domains_table, src, table_bytes);
  src += table_bytes;
  dst += table_bytes;

  db->domains_blob = (char*)dst;
  db->domains_blob_size = blob_bytes;
  memcpy(db->domains_blob, src, blob_bytes);
  src += blob_bytes;
  dst += blob_bytes;

  // Rebuild per-record base pointers and validate offsets.
  for (uint32_t b = 0; b < buckets; b++) {
    domains_table_record_t* rec = &db->domains_table[b];
    size_t base_off = rec->domains_blob_offset;
    if (base_off > db->domains_blob_size) {
      return HM_ERROR_BAD_VALUE;
    }
    rec->domains_blob = db->domains_blob + base_off;

    // Validate item offsets and NUL terminators inside blob bounds.
    for (uint32_t i = 0; i < rec->used_slots; i++) {
      size_t off_units = (size_t)rec->domains_offsets[i];
      size_t pos = base_off + off_units * D;
      if (pos + MAX_DOMAIN_LEN >= db->domains_blob_size) {
        return HM_ERROR_BAD_VALUE;
      }
    }
  }

  // Rebuild popular table base pointers and validate offsets.
  for (uint32_t r = 0; r < db->popular_records; r++) {
    domains_table_record_t* rec = &db->popular_table[r];
    size_t base_off = rec->domains_blob_offset;
    if (base_off > db->domains_blob_size) {
      return HM_ERROR_BAD_VALUE;
    }
    rec->domains_blob = db->domains_blob + base_off;
    for (uint32_t i = 0; i < rec->used_slots; i++) {
      size_t off_units = (size_t)rec->domains_offsets[i];
      size_t pos = base_off + off_units * D;
      if (pos + MAX_DOMAIN_LEN >= db->domains_blob_size) {
        return HM_ERROR_BAD_VALUE;
      }
    }
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
