// Benchmark tool for domain_to_lower validation + lowercasing.
// Usage:
//   bench_lower -text=text.csv [-n=10]
// Loads hosts from the input file (same format as bench_domains: lines like
// "<url>,<count>") and extracts hostnames. Then runs domain_to_lower on every
// hostname N times (default 10) to measure average latency per call.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "domain_to_lower.h"

namespace {

static inline void rtrim_trailing_dots(std::string& s) {
  while (!s.empty() && s.back() == '.') {
    s.pop_back();
  }
}

// Extract hostname (lowercase, no port) from a URL-ish string.
// - Cut at first comma.
// - Ensure scheme by adding http:// if missing.
// - Parse host as substring between scheme and next '/' or end.
// - Strip port, trim trailing dots.
static bool extract_host(const std::string& line, std::string& host_out) {
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

  std::string url = s;
  if (url.find("://") == std::string::npos) {
    url = std::string("http://") + url;
  }
  size_t scheme_pos = url.find("://");
  if (scheme_pos == std::string::npos) {
    return false;
  }
  size_t host_begin = scheme_pos + 3;
  if (host_begin >= url.size()) {
    return false;
  }
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
  host_out = host;
  return true;
}

static bool read_text_hosts(const std::string& path,
                            std::vector<std::string>& hosts) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::string host;
    if (!extract_host(line, host)) {
      continue;
    }
    hosts.emplace_back(std::move(host));
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string text_path;
  int N = 10;

  // Flags
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (strncmp(a, "-text=", 6) == 0) {
      text_path.assign(a + 6);
    } else if (strncmp(a, "-n=", 3) == 0) {
      N = std::atoi(a + 3);
      if (N <= 0) {
        N = 10;
      }
    } else if ((strcmp(a, "-h") == 0) || (strcmp(a, "--help") == 0)) {
      std::fprintf(stderr, "usage: bench_lower -text=text.csv [-n=10]\n");
      return 2;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a);
      return 2;
    }
  }
  if (text_path.empty()) {
    std::fprintf(stderr, "usage: bench_lower -text=text.csv [-n=10]\n");
    return 2;
  }

  std::cout << "Loading text: " << text_path << "..." << std::endl;
  std::vector<std::string> hosts;
  if (!read_text_hosts(text_path, hosts) || hosts.empty()) {
    std::fprintf(stderr, "failed to read text hosts or none loaded\n");
    return 1;
  }
  std::cout << "Loaded hosts: " << hosts.size() << std::endl;

  // Baseline: memory-touch only (sum first chars), two runs, average per-call
  volatile uint64_t baseline_sink = 0;
  double baseline_ns = 0.0;
  {
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
  }
  std::printf("Baseline per-call latency: %.3f ns (inputs=%zu)\n", baseline_ns,
              hosts.size());

  // Warm-up and measurement
  const size_t total_calls = (size_t)N * hosts.size();
  std::cout << "Running domain_to_lower: N=" << N << ", inputs=" << hosts.size()
            << " " << std::flush;
  uint64_t ok_count = 0;
  uint64_t fail_count = 0;
  std::vector<double> per_attempt_ns_per_call;
  per_attempt_ns_per_call.reserve(N);
  for (int iter = 0; iter < N; ++iter) {
    auto a0 = std::chrono::steady_clock::now();
    std::cout << '.' << std::flush;
    for (const auto& h : hosts) {
      char dst[256];
      bool ok = domain_to_lower(dst, h.c_str(), h.size());
      if (ok) {
        ok_count++;
      } else {
        fail_count++;
      }
    }
    auto a1 = std::chrono::steady_clock::now();
    std::chrono::duration<double> at = a1 - a0;
    double ns_per_call = (at.count() * 1e9) / (double)hosts.size();
    double adj = ns_per_call - baseline_ns;
    per_attempt_ns_per_call.push_back(adj < 0 ? 0.0 : adj);
  }
  std::cout << std::endl;
  // Aggregate stats
  double sum = 0.0;
  double minv =
      per_attempt_ns_per_call.empty() ? 0.0 : per_attempt_ns_per_call[0];
  double maxv = minv;
  for (double v : per_attempt_ns_per_call) {
    sum += v;
    if (v < minv) {
      minv = v;
    }
    if (v > maxv) {
      maxv = v;
    }
  }
  double avg = per_attempt_ns_per_call.empty()
                   ? 0.0
                   : (sum / (double)per_attempt_ns_per_call.size());
  std::vector<double> sorted = per_attempt_ns_per_call;
  std::sort(sorted.begin(), sorted.end());
  double med = 0.0;
  if (!sorted.empty()) {
    size_t m = sorted.size() / 2;
    if (sorted.size() % 2 == 1) {
      med = sorted[m];
    } else {
      med = 0.5 * (sorted[m - 1] + sorted[m]);
    }
  }

  std::cout << "Finished attempts: N=" << N << " s, total_calls=" << total_calls
            << ", ok=" << ok_count << ", fail=" << fail_count << std::endl;
  std::printf(
      "domain_to_lower per-call latency (ns): avg=%.3f min=%.3f median=%.3f "
      "max=%.3f (N=%d, inputs=%zu)\n",
      avg, minv, med, maxv, N, hosts.size());

  return 0;
}
