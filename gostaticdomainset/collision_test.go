package gostaticdomainset

import (
	"fmt"
	"testing"

	"github.com/stretchr/testify/require"
)

// TestPopularCollision_Repro tries to reproduce the collision-induced mismatch
// described in the bug report: when a domain's two-label suffix happens to
// collide with a popular hash, Find grows the suffix and may probe a different
// bucket than where the compiler placed it. This test searches for such a
// colliding candidate and expects a mismatch (fast != naive) to highlight the bug.
// NOTE: This test intentionally fails if no mismatch is observed; it serves as
// a reproduction harness and may be updated once the bug is fixed.
func TestPopularCollision_Repro(t *testing.T) {
	base := "popular.example.com"
	n := 40 // > D (16) to make base popular by subdomains

	// Build many subdomains of base, but not the base itself.
	var patterns []string
	for i := 0; i < n; i++ {
		patterns = append(patterns, fmt.Sprintf("x%d.%s", i, base))
	}

	cases := []struct {
		unrelated string
		query     string
	}{
		{
			unrelated: "n1110yam.tld",
			query:     "n1110yam.tld",
		},
		{
			unrelated: "n1110yam.tld",
			query:     "a.n1110yam.tld",
		},
		{
			unrelated: "n1110yam.tld",
			query:     "cc.a.n1110yam.tld",
		},
		{
			unrelated: "a.n1110yam.tld",
			query:     "a.n1110yam.tld",
		},
		{
			unrelated: "a.n1110yam.tld",
			query:     "b.a.n1110yam.tld",
		},
		{
			unrelated: "a.n1110yam.tld",
			query:     "ba.n1110yam.tld",
		},
	}

	for _, tc := range cases {
		t.Run(fmt.Sprintf("unrelated=%s query=%s", tc.unrelated, tc.query), func(t *testing.T) {
			patterns2 := append(append([]string(nil), patterns...), tc.unrelated)
			ds2, err := Compile(patterns2)
			require.NoError(t, err)

			naive := NewNaiveDomainSet(patterns2)

			fast, err := ds2.Find(tc.query)
			require.NoError(t, err)

			ref, err := naive.Find(tc.query)
			require.NoError(t, err)

			require.Equal(t, ref, fast)
		})
	}
}
