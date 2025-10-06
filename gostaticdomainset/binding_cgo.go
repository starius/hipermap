//go:build !use_pure_gostaticdomainset
// +build !use_pure_gostaticdomainset

package gostaticdomainset

import "unsafe"

// #include <stdlib.h>
// #include <hipermap/static_domain_set.h>
import "C"

// Hash16SuffixCI exposes the C hash16 function (lower 16 bits) for tests and tools.
// seed must match the database seed for comparable results.
// Deprecated.
func Hash16SuffixCI(s string, seed uint32) uint16 {
	return uint16(C.hm_domain_hash(
		(*C.char)(unsafe.Pointer(unsafe.StringData(s))),
		C.size_t(len(s)),
		C.uint64_t(uint64(seed)),
	))
}

// Hash64SpanCI returns the 64-bit case-insensitive hash of s with a 64-bit seed.
func Hash64SpanCI(s string, seed uint64) uint64 {
	if len(s) == 0 {
		return uint64(C.hm_domain_hash64_span_ci(nil, 0, C.uint64_t(seed)))
	}
	// Lowercase ASCII letters to make hashing case-insensitive for tests/tools.
	// Allocate a copy only if needed.
	// Note: Only 'A'-'Z' are folded; other bytes unchanged.
	var b []byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'A' && c <= 'Z' {
			if b == nil {
				b = []byte(s)
			}
			b[i] = c | 0x20
		} else if b != nil {
			b[i] = c
		}
	}
	if b == nil {
		return uint64(C.hm_domain_hash64_span_ci(
			(*C.char)(unsafe.Pointer(unsafe.StringData(s))),
			C.size_t(len(s)),
			C.uint64_t(seed),
		))
	}
	return uint64(C.hm_domain_hash64_span_ci(
		(*C.char)(unsafe.Pointer(&b[0])),
		C.size_t(len(b)),
		C.uint64_t(seed),
	))
}

// Hash64SpanCIFromBytes hashes a subslice b[off:off+n] with a 64-bit seed.
func Hash64SpanCIFromBytes(b []byte, off, n int, seed uint64) uint64 {
	if n == 0 {
		return uint64(C.hm_domain_hash64_span_ci(nil, 0, C.uint64_t(seed)))
	}
	if off < 0 || n < 0 || off+n > len(b) {
		panic("Hash64SpanCIFromBytes: out of range slice")
	}
	// Lowercase into a temporary buffer.
	tmp := make([]byte, n)
	for i := 0; i < n; i++ {
		c := b[off+i]
		if c >= 'A' && c <= 'Z' {
			tmp[i] = c | 0x20
		} else {
			tmp[i] = c
		}
	}
	return uint64(C.hm_domain_hash64_span_ci(
		(*C.char)(unsafe.Pointer(&tmp[0])),
		C.size_t(n),
		C.uint64_t(seed),
	))
}

// CutLastDomainLabelOffset returns the start offset of the last label.
// Trailing dots are ignored. Empty string or single-label returns 0.
func CutLastDomainLabelOffset(s string) int {
	if len(s) == 0 {
		return 0
	}
	off := C.hm_cut_last_domain_label_offset(
		(*C.char)(unsafe.Pointer(unsafe.StringData(s))),
		C.size_t(len(s)),
	)
	return int(off)
}
