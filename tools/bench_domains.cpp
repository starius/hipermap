// Benchmark tool for StaticDomainSet vs naive matcher.
// Usage:
//   bench_domains -patterns=patterns.txt -text=text.csv [-n=10] [-naive_n=2]
// Reads patterns (one domain per line; keeps up to first whitespace),
// and a text file with lines like "<url>,<count>"; extracts hostnames.
// Builds the optimized C++ static domain set and a naive C++ matcher.
// Runs N passes over all text domains calling find and times total duration.
// Prints average find-call latency in nanoseconds and match counters.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include "static_domain_set.h"
}

#ifdef BENCH_ENABLE_HYPERSCAN
#include "domain_map_db.h"
#endif

namespace {

static const char* hm_err_to_str(hm_error_t e) {
  switch (e) {
    case HM_SUCCESS:
      return "success";
    case HM_ERROR_BAD_ALIGNMENT:
      return "db_place bad alignment";
    case HM_ERROR_SMALL_PLACE:
      return "db_place too small";
    case HM_ERROR_NO_MASKS:
      return "no domains";
    case HM_ERROR_BAD_VALUE:
      return "invalid domain list";
    case HM_ERROR_BAD_RANGE:
      return "bad range";
    case HM_ERROR_BAD_SIZE:
      return "bad size";
    case HM_ERROR_TOO_MANY_POPULAR_DOMAINS:
      return "too many popular domains";
    case HM_ERROR_FAILED_TO_CALIBRATE:
      return "failed to calibrate";
    case HM_ERROR_TOP_LEVEL_DOMAIN:
      return "top-level domains are not supported";
    default:
      return "unknown error";
  }
}

static inline std::string ascii_lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

static inline void rtrim_trailing_dots(std::string& s) {
  while (!s.empty() && s.back() == '.') {
    s.pop_back();
  }
}

// Reads lines, trims, keeps content up to first whitespace, lowercases
// and strips trailing dots. Empty lines are skipped.
bool read_patterns(const std::string& path, std::vector<std::string>& out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    // Trim spaces
    size_t start = 0;
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t' || line[start] == '\r')) {
      start++;
    }
    size_t end = line.size();
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                           line[end - 1] == '\r')) {
      end--;
    }
    if (end <= start) {
      continue;
    }
    std::string s = line.substr(start, end - start);
    // Keep only up to first whitespace
    size_t ws = s.find_first_of(" \t");
    if (ws != std::string::npos) {
      s.resize(ws);
    }
    rtrim_trailing_dots(s);
    if (s.empty()) {
      continue;
    }
    out.emplace_back(ascii_lower(std::move(s)));
  }
  return true;
}

// Extract hostname (lowercase, no port) from a URL-ish string.
// - Cut at first comma.
// - Ensure scheme by adding http:// if missing.
// - Parse host as substring between scheme and next '/' or end.
// - Strip port, lowercase, and trailing dots.
bool extract_host(const std::string& line, std::string& host_out) {
  // Cut by comma
  size_t comma = line.find(',');
  std::string s = (comma == std::string::npos) ? line : line.substr(0, comma);
  // Trim
  size_t start = 0;
  while (start < s.size() &&
         (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) {
    start++;
  }
  size_t end = s.size();
  while (end > start &&
         (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
    end--;
  }
  if (end <= start) {
    return false;
  }
  s = s.substr(start, end - start);
  if (s.empty()) {
    return false;
  }

  // Ensure scheme
  std::string url = s;
  if (url.find("://") == std::string::npos) {
    url = std::string("http://") + url;
  }
  // Find scheme end
  size_t scheme_pos = url.find("://");
  if (scheme_pos == std::string::npos) {
    return false;
  }
  size_t host_begin = scheme_pos + 3;
  if (host_begin >= url.size()) {
    return false;
  }
  // Host ends at '/' or end
  size_t path_pos = url.find('/', host_begin);
  size_t host_end = (path_pos == std::string::npos) ? url.size() : path_pos;
  if (host_end <= host_begin) {
    return false;
  }
  std::string host = url.substr(host_begin, host_end - host_begin);
  // Strip port
  size_t colon = host.find(':');
  if (colon != std::string::npos) {
    host.resize(colon);
  }
  rtrim_trailing_dots(host);
  if (host.empty()) {
    return false;
  }
  host_out = ascii_lower(std::move(host));
  return true;
}

// Prepend as many single-letter subdomains (e.g., "a.") as possible
// without exceeding the 253-byte domain length limit. Cycles letters a..z.
static inline void prepend_short_subdomains(std::string& host) {
  const size_t kMax = 253;
  if (host.size() >= kMax) {
    return;
  }
  size_t remaining = kMax - host.size();
  size_t k = remaining / 2;  // each extra label adds 2 bytes: "a."
  if (k == 0) {
    return;
  }
  std::string prefix;
  prefix.reserve(k * 2);
  char c = 'a';
  for (size_t i = 0; i < k; ++i) {
    prefix.push_back(c);
    prefix.push_back('.');
    c = (c == 'z') ? 'a' : static_cast<char>(c + 1);
  }
  host = prefix + host;
}

bool read_text_hosts(const std::string& path, std::vector<std::string>& hosts,
                     bool add_pathological) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    line_no++;
    std::string host;
    if (!extract_host(line, host)) {
      // Skip malformed lines silently; match verify tool behavior of exiting on
      // error. For benchmarking, we prefer to skip.
      continue;
    }
    // Optional pathological input: add many short subdomains up to length
    // limit.
    if (add_pathological) {
      prepend_short_subdomains(host);
    }
    hosts.emplace_back(std::move(host));
  }
  return true;
}

// Naive matcher: any whole-label suffix match, case-insensitive; validates
// input.
static inline bool is_valid_domain_char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

struct NaiveMatcher {
  std::unordered_set<std::string> set;
  explicit NaiveMatcher(const std::vector<std::string>& patterns) {
    set.reserve(patterns.size());
    for (const auto& p : patterns) {
      if (!p.empty()) {
        set.insert(p);
      }
    }
  }
  // return: 1 found, 0 not found, -1 invalid input
  inline int find_rc(const std::string& domain) const {
    if (domain.empty() || domain.size() > 253) {
      return -1;
    }
    for (unsigned char c : domain) {
      if (!is_valid_domain_char(c)) {
        return -1;
      }
    }
    // Lowercase copy
    std::string s = ascii_lower(domain);
    size_t n = s.size();
    if (set.find(s) != set.end()) {
      return 1;
    }
    for (size_t i = 0; i < n; ++i) {
      if (s[i] == '.' && i + 1 < n) {
        if (set.find(s.substr(i + 1)) != set.end()) {
          return 1;
        }
      }
    }
    return 0;
  }
};

struct FastDB {
  std::vector<char> db_place;
  hm_domain_database_t* db = nullptr;

  hm_error_t build(const std::vector<std::string>& patterns) {
    if (patterns.empty()) {
      return HM_ERROR_NO_MASKS;
    }
    std::vector<const char*> cptrs;
    cptrs.reserve(patterns.size());
    for (const auto& s : patterns) {
      cptrs.push_back(s.c_str());
    }
    size_t place_size = hm_domain_db_place_size((const char**)cptrs.data(),
                                                (unsigned int)cptrs.size());
    if (place_size == 0) {
      return HM_ERROR_BAD_VALUE;
    }
    db_place.resize(place_size);
    hm_error_t err = hm_domain_compile((char*)db_place.data(), db_place.size(),
                                       &db, (const char**)cptrs.data(),
                                       (unsigned int)cptrs.size());
    return err;
  }

  std::vector<char> serialize_db() const {
    std::vector<char> out;
    if (!db) {
      return out;
    }
    size_t need = hm_domain_serialized_size(db);
    if (need == 0) {
      return out;
    }
    out.resize(need);
    if (hm_domain_serialize(out.data(), out.size(), db) != HM_SUCCESS) {
      out.clear();
    }
    return out;
  }

  bool deserialize_db(const std::vector<char>& ser) {
    if (ser.empty()) {
      return false;
    }
    size_t place_sz = 0;
    if (hm_domain_db_place_size_from_serialized(&place_sz, ser.data(),
                                                ser.size()) != HM_SUCCESS) {
      return false;
    }
    db_place.assign(place_sz, 0);
    return hm_domain_deserialize(db_place.data(), db_place.size(), &db,
                                 ser.data(), ser.size()) == HM_SUCCESS;
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string patterns_path;
  std::string text_path;
  int N = 10;
  int N_naive = 2;
  int N_hs = 4;  // default Hyperscan attempts
  bool add_pathological = false;

  // Simple flag parsing
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (strncmp(a, "-patterns=", 10) == 0) {
      patterns_path.assign(a + 10);
    } else if (strncmp(a, "-text=", 6) == 0) {
      text_path.assign(a + 6);
    } else if (strncmp(a, "-n=", 3) == 0) {
      N = std::atoi(a + 3);
      if (N <= 0) {
        N = 10;
      }
    } else if (strncmp(a, "-naive_n=", 9) == 0) {
      N_naive = std::atoi(a + 9);
      if (N_naive <= 0) {
        N_naive = 2;
      }
    } else if (strncmp(a, "-hs_n=", 6) == 0) {
      N_hs = std::atoi(a + 6);
      if (N_hs < 0) {
        N_hs = 0;
      }
    } else if (strcmp(a, "-pathological") == 0) {
      add_pathological = true;
    } else if (strncmp(a, "-pathological=", 14) == 0) {
      add_pathological = std::atoi(a + 14) != 0;
    } else if ((strcmp(a, "-h") == 0) || (strcmp(a, "--help") == 0)) {
      std::fprintf(stderr,
                   "usage: bench_domains -patterns=patterns.txt -text=text.csv "
                   "[-n=10] [-naive_n=2] [-hs_n=4] [-pathological]\n");
      return 2;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a);
      return 2;
    }
  }
  if (patterns_path.empty() || text_path.empty()) {
    std::fprintf(stderr,
                 "usage: bench_domains -patterns=patterns.txt -text=text.csv "
                 "[-n=10] [-naive_n=2] [-hs_n=4] [-pathological]\n");
    return 2;
  }

  std::cout << "Loading patterns: " << patterns_path << "..." << std::endl;
  std::vector<std::string> patterns;
  if (!read_patterns(patterns_path, patterns) || patterns.empty()) {
    std::fprintf(stderr, "failed to read patterns or none loaded\n");
    return 1;
  }
  std::cout << "Loaded patterns: " << patterns.size() << std::endl;

  std::cout << "Loading text: " << text_path << "..." << std::endl;
  auto t_load0 = std::chrono::steady_clock::now();
  std::vector<std::string> hosts;
  if (!read_text_hosts(text_path, hosts, add_pathological) || hosts.empty()) {
    std::fprintf(stderr, "failed to read text hosts or none loaded\n");
    return 1;
  }
  auto t_load1 = std::chrono::steady_clock::now();
  std::cout << "Loaded hosts: " << hosts.size()
            << (add_pathological ? " (with pathological prefixes)" : "")
            << std::endl;

  // Build fast DB
  std::cout << "Compiling fast DB..." << std::endl;
  auto t_compile0 = std::chrono::steady_clock::now();
  FastDB fast;
  hm_error_t build_err = fast.build(patterns);
  if (build_err != HM_SUCCESS) {
    std::fprintf(stderr, "failed to build fast DB: %s (%d)\n",
                 hm_err_to_str(build_err), (int)build_err);
    return 1;
  }
  auto t_compile1 = std::chrono::steady_clock::now();
  std::cout << "Compiled fast DB. buckets=" << hm_domain_buckets(fast.db)
            << " popular=" << hm_domain_popular_count(fast.db)
            << " used_total=" << hm_domain_used_total(fast.db) << " seed=0x"
            << std::hex << hm_domain_hash_seed(fast.db) << std::dec
            << " serialized_size=" << hm_domain_serialized_size(fast.db)
            << std::endl;
  std::cout << "Preparing queries..." << std::endl;
  std::vector<const char*> qptrs;
  std::vector<size_t> qlens;
  for (const auto& h : hosts) {
    qptrs.push_back(h.c_str());
    qlens.push_back(h.size());
  }

  // Deserialize a second DB instance from serialized bytes
  FastDB fast2;
  // Reuse serialized form from 'fast'
  auto t_ser0 = std::chrono::steady_clock::now();
  auto ser = fast.serialize_db();
  auto t_ser1 = std::chrono::steady_clock::now();
  if (ser.empty()) {
    std::fprintf(stderr, "failed to serialize fast DB\n");
    return 1;
  }
  auto t_deser0 = std::chrono::steady_clock::now();
  if (!fast2.deserialize_db(ser)) {
    std::fprintf(stderr, "failed to deserialize DB copy\n");
    return 1;
  }
  auto t_deser1 = std::chrono::steady_clock::now();

  std::chrono::duration<double> d_compile = t_compile1 - t_compile0;
  std::chrono::duration<double> d_ser = t_ser1 - t_ser0;
  std::chrono::duration<double> d_deser = t_deser1 - t_deser0;
  std::chrono::duration<double> d_load = t_load1 - t_load0;
  std::cout << "Timings: compile=" << (d_compile.count() * 1e3) << " ms"
            << ", serialize=" << (d_ser.count() * 1e3) << " ms"
            << ", deserialize=" << (d_deser.count() * 1e3) << " ms"
            << ", load_text=" << (d_load.count() * 1e3) << " ms" << std::endl;

  // Build naive DB
  std::cout << "Building naive matcher..." << std::endl;
  auto t_naive0 = std::chrono::steady_clock::now();
  NaiveMatcher naive(patterns);
  auto t_naive1 = std::chrono::steady_clock::now();
  std::cout << "Built naive matcher." << std::endl;

#ifdef BENCH_ENABLE_HYPERSCAN
  // Pre-build Hyperscan DB and scratch before timing loops
  DomainMapDB hsdb = nullptr;
  DomainMapDB hsdb2 = nullptr;
  DomainMapDBScratch hs_scratch = nullptr;
  DomainMapDBScratch hs_scratch2 = nullptr;
  if (N_hs > 0) {
    std::cout << "Building Hyperscan DB..." << std::endl;
    std::vector<const char*> hspats;
    hspats.reserve(patterns.size());
    for (const auto& p : patterns) {
      hspats.push_back(p.c_str());
    }
    hsdb = domain_map_db_build_from_patterns(hspats.data(),
                                             (uint32_t)hspats.size());
    if (!hsdb) {
      std::fprintf(stderr, "Hyperscan: failed to build DB from patterns\n");
    } else {
      hsdb2 = domain_map_db_clone(hsdb);
      hs_scratch = domain_map_db_scratch_create_empty();
      hs_scratch2 = domain_map_db_scratch_create_empty();
      if (!hs_scratch || !hs_scratch2) {
        std::fprintf(stderr, "Hyperscan: failed to allocate scratch\n");
      } else {
        (void)domain_map_db_scratch_adjust_to_db(hs_scratch, hsdb);
        (void)domain_map_db_scratch_adjust_to_db(hs_scratch2, hsdb2);
      }
    }
  }
#endif

  // Count matches once per implementation for reporting.
  // No separate counting pass; counts will be accumulated in the timed loops.

  // Baseline: memory-touch only (sum first chars), two runs, average per-call
  volatile uint64_t baseline_sink = 0;
  double baseline_ns = 0.0;
  {
    auto bl0 = std::chrono::steady_clock::now();
    double bsum = 0.0;
    for (int r = 0; r < 2; ++r) {
      auto b0 = std::chrono::steady_clock::now();
      for (const auto& h : hosts) {
        if (!h.empty()) {
          baseline_sink += (unsigned char)h[0];
        }
      }
      auto b1 = std::chrono::steady_clock::now();
      std::chrono::duration<double> bt = b1 - b0;
      bsum += (bt.count() * 1e9) / (double)hosts.size();
    }
    baseline_ns = bsum / 2.0;
    auto bl1 = std::chrono::steady_clock::now();
    std::chrono::duration<double> d_bl = bl1 - bl0;
    std::cout << "Pre-run baseline prep: " << (d_bl.count() * 1e3) << " ms"
              << std::endl;
  }
  std::printf("Baseline per-call latency: %.3f ns (inputs=%zu)\n", baseline_ns,
              hosts.size());

  // Timed loops: fast
  const size_t total_calls = (size_t)N * hosts.size();
  std::cout << "Running fast: N=" << N << ", inputs=" << hosts.size()
            << std::endl;
  auto t0 = std::chrono::steady_clock::now();
  uint64_t fast_hits = 0;
  uint64_t fast_errs = 0;
  std::vector<double> fast_attempt_ns_per_call;
  fast_attempt_ns_per_call.reserve(N);
  for (int iter = 0; iter < N; ++iter) {
    auto a0 = std::chrono::steady_clock::now();
    std::cout << "  Fast attempt " << (iter + 1) << " of " << N << std::endl;
    // Odd-numbered attempts (1-based) use original; even-numbered use
    // deserialized copy
    const hm_domain_database_t* which_db =
        ((iter % 2) == 0) ? fast.db : fast2.db;
    for (size_t i = 0; i < hosts.size(); ++i) {
      int rc = hm_domain_find(which_db, qptrs[i], qlens[i]);
      if (rc < 0) {
        fast_errs++;
      } else if (rc == 1) {
        fast_hits++;
      }
    }
    auto a1 = std::chrono::steady_clock::now();
    std::chrono::duration<double> at = a1 - a0;
    double ns_per_call = (at.count() * 1e9) / (double)hosts.size();
    double adj = ns_per_call - baseline_ns;
    fast_attempt_ns_per_call.push_back(adj < 0 ? 0.0 : adj);
  }
  auto t1 = std::chrono::steady_clock::now();
  std::chrono::duration<double> fast_sec = t1 - t0;
  double fast_ns_per_call = (fast_sec.count() * 1e9) / (double)total_calls;
  double fast_pct_total = (double)fast_hits * 100.0 / (double)total_calls;
  std::cout << "Finished fast in " << fast_sec.count()
            << " s, total_calls=" << total_calls << ", hits=" << fast_hits
            << " (" << std::fixed << std::setprecision(3) << fast_pct_total
            << "%)"
            << ", errors=" << fast_errs << std::defaultfloat << std::endl;

  // Timed loops: naive
  const size_t naive_total_calls = (size_t)N_naive * hosts.size();
  std::cout << "Running naive: N=" << N_naive << ", inputs=" << hosts.size()
            << std::endl;
  auto n0 = std::chrono::steady_clock::now();
  uint64_t naive_hits = 0;
  uint64_t naive_errs = 0;
  std::vector<double> naive_attempt_ns_per_call;
  naive_attempt_ns_per_call.reserve(N_naive);
  for (int iter = 0; iter < N_naive; ++iter) {
    auto a0 = std::chrono::steady_clock::now();
    std::cout << "  Naive attempt " << (iter + 1) << " of " << N_naive
              << std::endl;
    for (const auto& h : hosts) {
      int rc = naive.find_rc(h);
      if (rc < 0) {
        naive_errs++;
      } else if (rc == 1) {
        naive_hits++;
      }
    }
    auto a1 = std::chrono::steady_clock::now();
    std::chrono::duration<double> at = a1 - a0;
    double ns_per_call = (at.count() * 1e9) / (double)hosts.size();
    double adj = ns_per_call - baseline_ns;
    naive_attempt_ns_per_call.push_back(adj < 0 ? 0.0 : adj);
  }
  auto n1 = std::chrono::steady_clock::now();
  std::chrono::duration<double> naive_sec = n1 - n0;
  double naive_ns_per_call =
      (naive_sec.count() * 1e9) / (double)naive_total_calls;
  double naive_pct_total =
      (double)naive_hits * 100.0 / (double)naive_total_calls;
  std::cout << "Finished naive in " << naive_sec.count()
            << " s, total_calls=" << naive_total_calls
            << ", hits=" << naive_hits << " (" << std::fixed
            << std::setprecision(3) << naive_pct_total << "%)"
            << ", errors=" << naive_errs << std::defaultfloat << std::endl;

  // Hyperscan reporting moved after compute_stats is defined (below)

  // Output
  uint64_t fast_matches_per_pass = (uint64_t)(fast_hits / (uint64_t)N);
  uint64_t naive_matches_per_pass = (uint64_t)(naive_hits / (uint64_t)N_naive);
  double fast_pct_per_pass =
      (double)fast_matches_per_pass * 100.0 / (double)hosts.size();
  double naive_pct_per_pass =
      (double)naive_matches_per_pass * 100.0 / (double)hosts.size();
  auto compute_stats = [](const std::vector<double>& v, double& avg, double& mn,
                          double& med, double& mx) {
    if (v.empty()) {
      avg = mn = med = mx = 0.0;
      return;
    }
    double sum = 0.0;
    mn = v[0];
    mx = v[0];
    for (double x : v) {
      sum += x;
      if (x < mn) {
        mn = x;
      }
      if (x > mx) {
        mx = x;
      }
    }
    avg = sum / (double)v.size();
    std::vector<double> tmp = v;
    std::sort(tmp.begin(), tmp.end());
    size_t m = tmp.size() / 2;
    med = (tmp.size() % 2) ? tmp[m] : 0.5 * (tmp[m - 1] + tmp[m]);
  };
  double favg, fminv, fmed, fmaxv;
  compute_stats(fast_attempt_ns_per_call, favg, fminv, fmed, fmaxv);
  std::printf(
      "Fast:  per-call latency (ns): avg=%.3f min=%.3f median=%.3f max=%.3f "
      "(N=%d, inputs=%zu)\n",
      favg, fminv, fmed, fmaxv, N, hosts.size());
  std::printf(
      "       matches (per pass) = %llu (%.3f%%), total=%llu (%.3f%%), "
      "errors=%llu\n",
      (unsigned long long)fast_matches_per_pass, fast_pct_per_pass,
      (unsigned long long)fast_hits, fast_pct_total,
      (unsigned long long)fast_errs);
  double navg, nminv, nmed, nmaxv;
  compute_stats(naive_attempt_ns_per_call, navg, nminv, nmed, nmaxv);
  std::printf(
      "Naive: per-call latency (ns): avg=%.3f min=%.3f median=%.3f max=%.3f "
      "(N=%d, inputs=%zu)\n",
      navg, nminv, nmed, nmaxv, N_naive, hosts.size());
  std::printf(
      "       matches (per pass) = %llu (%.3f%%), total=%llu (%.3f%%), "
      "errors=%llu\n",
      (unsigned long long)naive_matches_per_pass, naive_pct_per_pass,
      (unsigned long long)naive_hits, naive_pct_total,
      (unsigned long long)naive_errs);
  // Divergence expected due to current popular collision bug.

  (void)fast_hits;  // avoid unused warnings if compiled with -Wall
  (void)naive_hits;

#ifdef BENCH_ENABLE_HYPERSCAN
  if (N_hs > 0) {
    // Use pre-built Hyperscan DB and scratch from above
    if (!hsdb || !hsdb2 || !hs_scratch || !hs_scratch2) {
      std::fprintf(stderr, "Hyperscan: not initialized; skipping runs\n");
    } else {
      const size_t hs_total_calls = (size_t)N_hs * hosts.size();
      std::cout << "Running Hyperscan: N=" << N_hs
                << ", inputs=" << hosts.size() << std::endl;
      auto h0 = std::chrono::steady_clock::now();
      uint64_t hs_hits = 0;
      uint64_t hs_errs = 0;
      std::vector<double> hs_attempt_ns_per_call;
      hs_attempt_ns_per_call.reserve(N_hs);
      for (int iter = 0; iter < N_hs; ++iter) {
        auto a0 = std::chrono::steady_clock::now();
        std::cout << "  Hyperscan attempt " << (iter + 1) << " of " << N_hs
                  << std::endl;
        DomainMapDB use = ((iter % 2) == 0) ? hsdb : hsdb2;
        DomainMapDBScratch use_s = ((iter % 2) == 0) ? hs_scratch : hs_scratch2;
        for (const auto& h : hosts) {
          int rc = domain_map_db_match(use, use_s, h.c_str());
          if (rc < 0) {
            hs_errs++;
          } else if (rc == 1) {
            hs_hits++;
          }
        }
        auto a1 = std::chrono::steady_clock::now();
        std::chrono::duration<double> at = a1 - a0;
        double ns_per_call = (at.count() * 1e9) / (double)hosts.size();
        double adj = ns_per_call - baseline_ns;
        hs_attempt_ns_per_call.push_back(adj < 0 ? 0.0 : adj);
      }
      auto h1 = std::chrono::steady_clock::now();
      std::chrono::duration<double> hs_sec = h1 - h0;
      double hs_ns_per_call = (hs_sec.count() * 1e9) / (double)hs_total_calls;
      double hs_pct_total = (double)hs_hits * 100.0 / (double)hs_total_calls;
      std::cout << "Finished Hyperscan in " << hs_sec.count()
                << " s, total_calls=" << hs_total_calls << ", hits=" << hs_hits
                << " (" << std::fixed << std::setprecision(3) << hs_pct_total
                << "%)"
                << ", errors=" << hs_errs << std::defaultfloat << std::endl;
      double havg, hminv, hmed, hmaxv;
      compute_stats(hs_attempt_ns_per_call, havg, hminv, hmed, hmaxv);
      std::printf(
          "HS:    per-call latency (ns): avg=%.3f min=%.3f median=%.3f "
          "max=%.3f (N=%d, inputs=%zu)\n",
          havg, hminv, hmed, hmaxv, N_hs, hosts.size());
    }
    // Cleanup pre-built Hyperscan resources
    if (hs_scratch) {
      domain_map_db_scratch_destroy(hs_scratch);
    }
    if (hs_scratch2) {
      domain_map_db_scratch_destroy(hs_scratch2);
    }
    if (hsdb2) {
      domain_map_db_destroy(hsdb2);
    }
    if (hsdb) {
      domain_map_db_destroy(hsdb);
    }
  }
#endif

  return 0;
}
