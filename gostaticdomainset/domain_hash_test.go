package gostaticdomainset

import (
	"bytes"
	"testing"

	"github.com/stretchr/testify/require"
)

// Gold 64-bit hash values for selected inputs at a fixed seed.
// Seed: 0x1122334455667788
var goldHashes = map[string]uint64{
	"":                  0xe0a68475e02b3edd,
	"com":               0x905158f655be1ad6,
	"google":            0x21f9955fe590aeed,
	"google.com":        0x8c157532763b8481,
	"images":            0x0d243944709d5a5e,
	"images.google.com": 0x65c751b699134471,
	"a":                 0xecac24523f4003c6,
	"A":                 0xecac24523f4003c6,
	"abc":               0xdf0420340b11b19e,
	"AbC":               0xdf0420340b11b19e,
	"xn--puny":          0xd6f6a38651a65dad,
	"xn--punycode":      0x4e7d7e7f53d315a1,
	"12345":             0xa62be382ce514675,
	"a.b":               0xe5c78ccdbdfb2d35,
	"zz.zz":             0xde7fbc749c9dcd26,
}

const goldSeed uint64 = 0x1122334455667788

func TestDomainHash_Gold(t *testing.T) {
	for s, want := range goldHashes {
		got := Hash64SpanCI(s, goldSeed)
		require.Equalf(t, want, got, "mismatch for %q", s)
	}
}

func TestDomainHash_AlignmentAndSurroundings(t *testing.T) {
	// Try each input with varying left/right padding to shake alignment.
	for s, want := range goldHashes {
		// Build a deterministic padding pattern.
		for left := 0; left < 32; left += 5 { // sample a few offsets up to 31
			for right := 0; right < 16; right += 3 {
				buf := make([]byte, left+len(s)+right)
				// left pad with '!','@','#',...
				for i := 0; i < left; i++ {
					buf[i] = byte('!' + (i % 10))
				}
				copy(buf[left:left+len(s)], []byte(s))
				// right pad with '~','}',... to ensure they are ignored
				for i := 0; i < right; i++ {
					buf[left+len(s)+i] = byte('~' - byte(i%10))
				}

				got := Hash64SpanCIFromBytes(buf, left, len(s), goldSeed)
				require.Equalf(t, want, got, "align mismatch for %q (L=%d R=%d)", s, left, right)
			}
		}
	}
}

func TestDomainHash_CaseInsensitivity(t *testing.T) {
	pairs := [][2]string{{"a", "A"}, {"abc", "AbC"}, {"images.google.com", "IMAGES.Google.Com"}}
	for _, p := range pairs {
		h1 := Hash64SpanCI(p[0], goldSeed)
		h2 := Hash64SpanCI(p[1], goldSeed)
		require.Equalf(t, h1, h2, "case-insensitive mismatch: %q vs %q", p[0], p[1])
	}
}

func TestDomainHash_SubsliceNoCopy(t *testing.T) {
	// Ensure hashing from a subslice of a larger buffer returns the same result
	// as hashing the isolated string.
	s := "images.google.com"
	want := goldHashes[s]
	leftPad := bytes.Repeat([]byte{'X'}, 17)
	rightPad := bytes.Repeat([]byte{'Z'}, 9)
	buf := append(append(append([]byte{}, leftPad...), []byte(s)...), rightPad...)
	got := Hash64SpanCIFromBytes(buf, len(leftPad), len(s), goldSeed)
	require.Equal(t, want, got)
}

func TestDomainHash_SamplePartsAlignment(t *testing.T) {
	// For a subset of sample domains, verify that hashing any label is
	// invariant to surrounding bytes and starting alignment.
	max := 50
	if len(sampleDomainStrings) < max {
		max = len(sampleDomainStrings)
	}
	for _, dom := range sampleDomainStrings[:max] {
		parts := bytes.Split([]byte(dom), []byte{'.'})
		for _, p := range parts {
			s := string(p)
			want := Hash64SpanCI(s, goldSeed)
			for left := 0; left < 32; left += 7 {
				for right := 0; right < 16; right += 5 {
					buf := make([]byte, left+len(s)+right)
					for i := 0; i < left; i++ {
						buf[i] = byte('0' + byte(i%10))
					}
					copy(buf[left:left+len(s)], s)
					for i := 0; i < right; i++ {
						buf[left+len(s)+i] = byte('a' + byte(i%26))
					}
					got := Hash64SpanCIFromBytes(buf, left, len(s), goldSeed)
					require.Equalf(t, want, got, "label=%q dom=%q L=%d R=%d", s, dom, left, right)
				}
			}
		}
	}
}
