puregostaticdomainset
=====================

`puregostaticdomainset` is a pure Go implementation of hipermap’s static domain
set. It mirrors the C data layout and hashing strategy, allowing databases to be
compiled in Go and consumed by the C backend (and vice versa).

Public API
----------

The package exposes the following entry points:

- `Compile(domains []string) (*StaticDomainSet, error)` builds a database from a
  slice of ASCII domains (trailing dots are normalised; top-level domains are
  rejected).
- `(s *StaticDomainSet) Find(domain string) (bool, error)` matches a query
  domain, returning true when any whole-label suffix is present.
- `(s *StaticDomainSet) Serialize() ([]byte, error)` encodes the database into
  the same byte format used by the C implementation.
- `FromSerialized(buf []byte) (*StaticDomainSet, error)` reconstructs a set from
  a serialized buffer.
- `(s *StaticDomainSet) Seed() uint32` and `(s *StaticDomainSet) String()
  string` expose metadata useful for diagnostics;
  `(s *StaticDomainSet) Allocated() int` reports the materialised size in
  bytes.

Documentation
-------------

Complete Go documentation is published at:
https://pkg.go.dev/github.com/starius/hipermap/puregostaticdomainset

The package is designed to be drop-in compatible with the cgo bindings found in
`gostaticdomainset/`.
