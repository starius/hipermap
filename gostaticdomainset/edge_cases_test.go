package gostaticdomainset

import (
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
)

// Constructs a domain with a single label of given length followed by .com.
func makeLabelN(n int) string {
	if n <= 0 {
		return "a.com"
	}
	return strings.Repeat("a", n) + ".com"
}

func TestLongLabels63AndMore(t *testing.T) {
	// 63 is the classic DNS label limit; implementation does not enforce it,
	// so 64+ should still compile and behave consistently.
	l63 := makeLabelN(63)
	l64 := makeLabelN(64)
	l200 := makeLabelN(200) // stresses cutters and hashing

	ds, err := Compile([]string{l63, l64, l200})
	require.NoError(t, err)

	// Exact lookups
	for _, q := range []string{l63, l64, l200} {
		ok, err := ds.Find(q)
		require.NoError(t, err, q)
		require.True(t, ok, q)
	}

	// With one subdomain added (suffix semantics)
	for _, q := range []string{"x." + l63, "y." + l64, "z." + l200} {
		ok, err := ds.Find(q)
		require.NoError(t, err, q)
		require.True(t, ok, q)
	}

	// With multiple trailing dots
	for _, q := range []string{l63 + ".", l64 + "..", l200 + "..."} {
		ok, err := ds.Find(q)
		require.NoError(t, err, q)
		require.True(t, ok, q)
	}
}

func TestTrailingAndLeadingDotsLookup(t *testing.T) {
	ds, err := Compile([]string{"example.com", "a..b.com"})
	require.NoError(t, err)

	queries := []string{
		// Trailing dots (1..n)
		"example.com.",
		"example.com..",
		"example.com...",
		// Leading dots should not prevent matching of label suffix
		".example.com",
		"..example.com",
		// Mixed leading+trailing
		".example.com.",
		"..example.com...",
		// Internal empty labels also supported by the implementation
		"a..b.com",
		"x.a..b.com",
		"a..b.com.",
	}
	for _, q := range queries {
		ok, err := ds.Find(q)
		// Inputs are ASCII and <=253 after trimming trailing dots, so expect no error.
		require.NoError(t, err, q)
		require.True(t, ok, q)
	}
}

func TestHyphenEdges(t *testing.T) {
	// Hyphens at label edges are permitted by this implementation (no RFC enforcement)
	domains := []string{"-start.com", "end-.com", "mi-d.le-.ex-ample.com"}
	ds, err := Compile(domains)
	require.NoError(t, err)

	for _, d := range domains {
		ok, err := ds.Find(d)
		require.NoError(t, err, d)
		require.True(t, ok, d)
		// Subdomain should also match via suffix
		q := "x." + d
		ok, err = ds.Find(q)
		require.NoError(t, err, q)
		require.True(t, ok, q)
	}
}

func TestSuffixNotOnLabelBoundary(t *testing.T) {
	// Ensure matches are only on whole-label boundaries.
	ds, err := Compile([]string{"ample.com"})
	require.NoError(t, err)

	// example.com ends with "ample.com" but not on a label boundary -> false
	ok, err := ds.Find("example.com")
	require.NoError(t, err)
	require.False(t, ok)

	// Boundary case: ".ample.com" should match
	ok, err = ds.Find("x.ample.com")
	require.NoError(t, err)
	require.True(t, ok)
}

func TestMaxLenQueryAndNonASCIIQuery(t *testing.T) {
	// Build a domain exactly at 253 bytes (valid)
	// Construct: 250 'a' + ".com" (total 254) is invalid at compile, so use 249 + ".com" = 253
	base := strings.Repeat("a", 249) + ".com"
	require.Equal(t, 253, len(base))

	ds, err := Compile([]string{base})
	require.NoError(t, err)

	ok, err := ds.Find(base)
	require.NoError(t, err)
	require.True(t, ok)

	// Non-ASCII query should return an error, not crash
	_, err = ds.Find("пример.рф")
	require.Error(t, err)
}
