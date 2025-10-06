//go:build use_pure_gostaticdomainset
// +build use_pure_gostaticdomainset

package gostaticdomainset

import (
	"github.com/zeebo/xxh3"
)

// Hash16SuffixCI exposes the internal 16-bit hash used by the pure builder.
func Hash16SuffixCI(s string, seed uint32) uint16 {
	return uint16(hash64SpanCIString(s, uint64(seed)) & 0xFFFF)
}

// Hash64SpanCI returns the 64-bit case-insensitive hash of s with a 64-bit seed.
func Hash64SpanCI(s string, seed uint64) uint64 {
	return hash64SpanCIString(s, seed)
}

// Hash64SpanCIFromBytes hashes a subslice b[off:off+n] with a 64-bit seed.
func Hash64SpanCIFromBytes(b []byte, off, n int, seed uint64) uint64 {
	if n == 0 {
		return hash64SpanCI(nil, seed)
	}
	if off < 0 || n < 0 || off+n > len(b) {
		panic("Hash64SpanCIFromBytes: out of range slice")
	}
	buf := make([]byte, n)
	for i := 0; i < n; i++ {
		c := b[off+i]
		if c >= 'A' && c <= 'Z' {
			buf[i] = c | 0x20
		} else {
			buf[i] = c
		}
	}
	return hash64SpanCI(buf, seed)
}

// CutLastDomainLabelOffset returns the start offset of the last label.
// Empty string or single-label returns 0.
func CutLastDomainLabelOffset(s string) int {
	for i := len(s) - 1; i >= 0; i-- {
		if s[i] == '.' {
			return i + 1
		}
	}
	return 0
}

func hash64SpanCIString(s string, seed uint64) uint64 {
	if len(s) == 0 {
		return hash64SpanCI(nil, seed)
	}
	var buf []byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'A' && c <= 'Z' {
			if buf == nil {
				buf = []byte(s)
			}
			buf[i] = c | 0x20
		} else if buf != nil {
			buf[i] = c
		}
	}
	if buf == nil {
		return hash64SpanCI([]byte(s), seed)
	}
	return hash64SpanCI(buf, seed)
}

func hash64SpanCI(b []byte, seed uint64) uint64 {
	return xxh3.HashSeed(b, seed)
}
