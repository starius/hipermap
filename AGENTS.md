# Repository Guidelines

## Overview
High‑performance static domain set with a stable C API, C++ builder, and Go bindings. Tools include a domain benchmark and optional Hyperscan comparison.

## Layout
- C/C++ core (root):
  - Builder: `static_domain_set.cpp`
  - Hot path: `static_domain_set_find.c`
  - Shared types/layout: `static_domain_set_internal.h`
  - Utilities: `static_uint64_set.*`, `static_uint64_map.*`, `cache.*`, `common.h`, `fastmod.h`, `domain_to_lower.h`, `domain_hash.h`, `xxhash.h`
- Go bindings + tests: `gostaticdomainset/` (tools under `cmd/`)
- Build tooling: `CMakeLists.txt`, `flake.nix`, `Makefile`

## Build Matrix
- Nix packages:
  - `.#musl` (default; fully static via musl; Hyperscan OFF)
  - `.#glibc` (dynamic; Hyperscan ON for domain bench)
  - `.#debug`, `.#sanitizers`, `.#avx512`
  - Cross: `.#arm64` (aarch64‑musl, static), `.#mingw64` (Windows x86_64, static)
- Usage:
  - `nix build -L .#glibc -o out-glibc` and run binaries from `out-*`/bin
- Makefile shortcuts:
  - `make fmt | build | test`
  - `make nix-{musl,glibc,debug,sanitizers,avx512,arm64,mingw64}` or `make nix`
- Local CMake:
  - `cmake -S . -B build && cmake --build build -j`
  - Options: `-DHIPERMAP_ENABLE_SANITIZERS=ON`, `-DHIPERMAP_ENABLE_AVX512=ON`, `-DHIPERMAP_DOMAIN_BENCH_ENABLE_HYPERSCAN=ON`
  - Install: `cmake --install build --prefix build/out`

## Go + cgo
- CGO include/lib paths are provided by Nix/Make; sources don’t hardcode `-I/-L`.
- Tests: `go test ./...`; benches: `go test -bench=. -benchmem`.
- `make pure-test` runs both the cgo-backed and pure-Go gostaticdomainset suites (including cross-serialization checks).

## Pure Go Static Domain Set
- Location: `puregostaticdomainset/`
- Purpose: non‑SIMD, pure Go reimplementation of the static domain set (no unsafe), compatible with the cgo version.
- API:
  - `Compile(domains []string) (*StaticDomainSet, error)`
  - `Find(domain string) (bool, error)`
  - `Serialize() ([]byte, error)` and `FromSerialized([]byte)` — same on‑disk format as `gostaticdomainset`/C.
  - `String() string` — summary similar to `gostaticdomainset`.
- Behavior:
  - Uses the non‑SIMD algorithm: lowercase/validate, prune subdomains, discover popular suffixes, calibrate seed/buckets, bucket by `fastmod_u32`, scan with `max_scans` limits, extend popular suffixes leftward.
  - Hashing: `xxh3` (`github.com/zeebo/xxh3`), 64‑bit seed chaining, matching C.
  - Data layout mirrors C’s 64‑byte records and 64‑byte header; serialization is cross‑compatible.
- Building:
  - Run `go mod tidy` (adds `github.com/zeebo/xxh3`).
  - No cgo or platform SIMD required.
  - Serialized databases are cross-compatible with the cgo implementation and covered by `pure_compat_test`.
- Notes:
  - Popular suffix count is deduped/sorted; bucket capacity is enforced during calibration.
  - Intended for heavy testing and portability; performance targets the scalar C path.

## Domain Rules
- ASCII only; IDNs must be Punycode (`xn--...`).
- Valid chars: `[A-Za-z0-9.-]`; length ≤253, labels ≤63.
- Top‑level domains (no dot) are rejected at compile (`HM_ERROR_TOP_LEVEL_DOMAIN`).

## Matching Architecture (Summary)
- Lowercase and validate input; process labels right‑to‑left.
- Hashing: XXH3‑64 (seeded), incrementally chained.
- Table: array of 64‑byte records; map bucket with `fastmod_u32`.
- Popular suffixes stored separately; while suffix is popular (tag + string), extend left before bucketing.
- Compile calibrates buckets/seed to avoid overflow; caps per‑bucket scans via `max_scans`.

## SIMD Notes
- x86_64: AVX2 baseline; optional AVX‑512BW.
- ARM64: NEON; default avoids `-mcpu=native` for portability (opt‑in via `HIPERMAP_ARM64_TUNE_NATIVE`).
- Equality/path cutters use 16/32‑byte vector scans with safe padding.

## Serialization & Alignment
- On‑disk format is stable; do not change.
- Buffers can be only byte‑aligned — use `memcpy` for integral reads/writes.

## Undefined Behavior Guards
- Guard shift counts; use unsigned literals (e.g., `1u << 31`).
- Validate lengths before SIMD loads; maintain 32‑byte padding for vector compares.

## Style
- 2‑space indent; snake_case APIs (`hm_*`), typedefs `hm_*_t`, macros UPPER_SNAKE.
- Use `HM_PUBLIC_API`, `HM_CDECL` for exported C symbols.
- Place comments above struct fields; avoid end‑of‑line clutter.

## Tools
- `bench_domains` (domain bench): compares fast set vs naive (and optional Hyperscan).
  - Flags: `-patterns`, `-text`, `-n`, `-naive_n`, `-hs_n`, `-pathological[=0|1]`.
  - Enable Hyperscan with `-DHIPERMAP_DOMAIN_BENCH_ENABLE_HYPERSCAN=ON` (needs `libhs`).
- `static_map_benchmark` (optional): static IP map benchmark (requires Hyperscan + libcork + ipset).
- `test_cache`: cache throughput microbench (portable; Windows has a time shim).

## Header & Formatting
- C++ sources include C headers via C++ forms (`<cstdio>`, `<cstring>`, `<cinttypes>`), and list C headers before C++ headers.
- Public headers using `size_t` include `<stddef.h>`.
- Formatting: `make fmt` (clang‑format C/C++; go fmt; `nixfmt` for `flake.nix`).
