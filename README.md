# hipermap

hipermap is a collection of high-performance, read-only data structures for
security and networking workloads. It provides a static domain set matcher, a
static IPv4 prefix map, a compact uint64 set, and an LRU cache for IPv4 lookups.
Each C component exposes a stable C API; Go bindings are available for tests and
tooling.

## Components

### Static domain set (`static_domain_set.h`)
`hm_domain_database_t` stores a case-insensitive set of fully-qualified domain
names with fast suffix expansion for popular subtrees. Key entry points:

- `hm_domain_db_place_size(const char **domains, unsigned int elements)`
  computes the buffer size required to build a database for the given input
  domains.
- `hm_domain_compile(char *db_place, size_t db_place_size,
  hm_domain_database_t **db_ptr, const char **domains, unsigned int elements)`
  builds a lookup table inside user-provided storage.
- `hm_domain_find(const hm_domain_database_t *db, const char *domain,
  size_t domain_len)` returns 1 when the domain (or a matching suffix) is in the
  set, 0 when it is absent, and -1 on malformed input.
- Serialization helpers (`hm_domain_serialized_size`, `hm_domain_serialize`,
  `hm_domain_db_place_size_from_serialized`, `hm_domain_deserialize`) make it
  possible to persist a calibrated database and reload it in constant time.

The domain set format is mirrored by the pure Go implementation in
`puregostaticdomainset/`, allowing cross-language serialization.

### Static IPv4 prefix map (`static_map.h`)
`hm_sm_database_t` stores IPv4 prefixes (address + CIDR) mapped to 64-bit
values. Longest-prefix match is performed during lookup.

- `hm_sm_db_place_size(unsigned int elements)` sizes the storage buffer.
- `hm_sm_compile(char *db_place, size_t db_place_size,
  hm_sm_database_t **db_ptr, const uint32_t *ips,
  const uint8_t *cidr_prefixes, const uint64_t *values,
  unsigned int elements)` builds the map.
- `hm_sm_find(const hm_sm_database_t *db, uint32_t ip)` returns the associated
  value or `HM_NO_VALUE` when no prefix matches.
- Serialization helpers mirror those provided for the domain set to simplify
  persistence.

### Static uint64 set (`static_uint64_set.h`)
`hm_u64_database_t` stores a deduplicated set of 64-bit integers (1..2^64-1).

- `hm_u64_db_place_size(unsigned int elements)` and
  `hm_u64_compile(...)` build the set.
- `hm_u64_find(const hm_u64_database_t *db, uint64_t key)` tests membership.
- Serialization helpers allow persisting and restoring populated sets.

### IPv4 LRU cache (`cache.h`)
`hm_cache_t` is a bounded, least-recently-used cache keyed by IPv4 address with
user-provided 32-bit values.

- `hm_cache_place_size(size_t *cache_place_size, unsigned int capacity,
  int speed)` reports the storage requirement (capacity must be a power of two;
  speed controls the probe/footprint trade-off).
- `hm_cache_init(char *cache_place, size_t cache_place_size,
  hm_cache_t **cache_ptr, unsigned int capacity, int speed)` initialises the
  cache in caller-owned memory.
- `hm_cache_add`, `hm_cache_remove`, and `hm_cache_has` update and query cached
  entries. Optional out-parameters report evicted elements to facilitate
  back-population.

### Support headers
`common.h` defines the shared error codes (`hm_error_t`, `HM_SUCCESS`,
`HM_ERROR_*`) and calling conventions (`HM_CDECL`, `HM_PUBLIC_API`).
`domain_hash.h` exposes the internal XXH3 helper (`hash64_span_ci`) used by the
static domain set when integrating hipermap into custom pipelines.

## Dependencies
The project targets POSIX-like systems (Linux, macOS) and requires a C11
compiler, CMake (>=3.16 recommended), and Go 1.24 or newer for the auxiliary
bindings/tests. Debian/Ubuntu example:

```
sudo apt-get install build-essential cmake pkg-config golang
```

Optional features:
- Hyperscan integration for the domain benchmark (`libhs` headers and library).
- Clang/LLVM for clang-format (used by `make fmt`).

## Build, test, and install
The Makefile wraps the standard CMake workflow:

- `make build` configures CMake in `build/`, compiles the C/C++ targets,
  installs them into `build/out/`, and builds the Go tools.
- `make test` runs all Go test suites with CGO enabled (using any installed C
  artifacts).
- `make fmt` applies clang-format, gofmt, and nixfmt.

The CMake configuration honours several feature toggles:

- `-DHIPERMAP_ENABLE_SANITIZERS=ON` links AddressSanitizer and
  UndefinedBehaviorSanitizer.
- `-DHIPERMAP_ENABLE_AVX512=ON` enables AVX-512BW fast paths on supported CPUs.
- `-DHIPERMAP_DOMAIN_BENCH_ENABLE_HYPERSCAN=ON` links the optional Hyperscan
  comparison in the benchmarking tool.

Manual invocation example:

```
cmake -S . -B build -DHIPERMAP_ENABLE_SANITIZERS=ON
cmake --build build --parallel
cmake --install build --prefix build/out
```

## Nix builds
A flake is provided with several preconfigured outputs:

- `nix build -L .#musl` (fully static, musl-based toolchain)
- `nix build -L .#glibc` (dynamic build with Hyperscan for benchmarks)
- `.#debug`, `.#sanitizers`, `.#avx512` (specialised variants)
- Cross-compilation targets: `.#arm64`, `.#mingw64`
- `.#nosimd` disables SIMD acceleration for portability testing

Use `nix build -L .#target -o out-target` to create an output symlink for each
profile.

## Pure Go mirror
The static domain set logic is also implemented in pure Go under
[`puregostaticdomainset/`](puregostaticdomainset/). The package shares the binary format with the C
implementation and is suitable for portability tests or direct use where CGO is
undesirable. See that directory for more details and API documentation.
