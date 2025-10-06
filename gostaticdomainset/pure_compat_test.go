//go:build !use_pure_gostaticdomainset
// +build !use_pure_gostaticdomainset

package gostaticdomainset

import (
	"testing"

	puregostaticdomainset "github.com/starius/hipermap/puregostaticdomainset"
	"github.com/stretchr/testify/require"
)

func TestPureCGOSerializationCompatibility(t *testing.T) {
	domains := sampleDomainStrings

	cgoSet, err := Compile(domains)
	require.NoError(t, err)

	pureSet, err := puregostaticdomainset.Compile(domains)
	require.NoError(t, err)

	queries := buildCrossQueries(t, cgoSet, pureSet, domains)
	require.NotEmpty(t, queries)

	// Serialize from cgo, load in pure, compare lookups.
	cgoSer, err := cgoSet.Serialize()
	require.NoError(t, err)

	pureFromCgo, err := puregostaticdomainset.FromSerialized(cgoSer)
	require.NoError(t, err)

	for _, q := range queries {
		gotCgo, err := cgoSet.Find(q)
		require.NoErrorf(t, err, "cgo Find(%q)", q)
		gotPure, err := pureFromCgo.Find(q)
		require.NoErrorf(t, err, "pure(cgo) Find(%q)", q)
		require.Equalf(t, gotCgo, gotPure, "query %q mismatch after cgo -> pure serialization", q)
	}

	// Serialize from pure, load in cgo, compare lookups.
	pureSer, err := pureSet.Serialize()
	require.NoError(t, err)

	cgoFromPure, err := FromSerialized(pureSer)
	require.NoError(t, err)

	for _, q := range queries {
		gotPure, err := pureSet.Find(q)
		require.NoErrorf(t, err, "pure Find(%q)", q)
		gotCgo, err := cgoFromPure.Find(q)
		require.NoErrorf(t, err, "cgo(pure) Find(%q)", q)
		require.Equalf(t, gotPure, gotCgo, "query %q mismatch after pure -> cgo serialization", q)
	}
}

func buildCrossQueries(t *testing.T, cgoSet *StaticDomainSet, pureSet *puregostaticdomainset.StaticDomainSet, bases []string) []string {
	t.Helper()

	seen := make(map[string]struct{}, len(bases)*4)
	queries := make([]string, 0, len(bases)*4)

	for _, base := range bases {
		candidates := []string{
			base,
			addBenchmarkSubdomain(base),
			removeBenchmarkSubdomain(base),
			addBenchmarkLetter(base),
			removeBenchmarkLetter(base),
		}
		for _, candidate := range candidates {
			if candidate == "" {
				continue
			}
			if _, ok := seen[candidate]; ok {
				continue
			}
			if _, err := cgoSet.Find(candidate); err != nil {
				continue
			}
			if _, err := pureSet.Find(candidate); err != nil {
				continue
			}
			seen[candidate] = struct{}{}
			queries = append(queries, candidate)
		}
	}

	return queries
}
