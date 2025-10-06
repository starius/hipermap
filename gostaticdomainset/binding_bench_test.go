package gostaticdomainset

import (
	"strings"
	"testing"
)

func BenchmarkFind(b *testing.B) {
	ds, err := Compile(sampleDomainStrings)
	if err != nil {
		b.Fatalf("Compile: %v", err)
	}

	queries := buildFindBenchmarkQueries(b, ds, sampleDomainStrings)
	if len(queries) == 0 {
		b.Fatal("no benchmark queries")
	}

	totalBytes := 0
	for _, q := range queries {
		totalBytes += len(q)
	}
	avgBytes := totalBytes / len(queries)
	if avgBytes == 0 {
		avgBytes = 1
	}
	b.SetBytes(int64(avgBytes))

	b.ResetTimer()
	j := 0
	for i := 0; i < b.N; i++ {
		if j == len(queries) {
			j = 0
		}
		q := queries[j]
		if _, err := ds.Find(q); err != nil {
			b.Fatalf("Find(%q): %v", q, err)
		}
		j++
	}
}

func buildFindBenchmarkQueries(tb testing.TB, ds *StaticDomainSet, bases []string) []string {
	tb.Helper()

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
			if _, err := ds.Find(candidate); err != nil {
				continue
			}
			seen[candidate] = struct{}{}
			queries = append(queries, candidate)
		}
	}

	return queries
}

func addBenchmarkSubdomain(domain string) string {
	if domain == "" {
		return ""
	}
	return "bench." + domain
}

func removeBenchmarkSubdomain(domain string) string {
	idx := strings.IndexByte(domain, '.')
	if idx <= 0 || idx+1 >= len(domain) {
		return ""
	}
	return domain[idx+1:]
}

func addBenchmarkLetter(domain string) string {
	if domain == "" {
		return ""
	}
	idx := strings.IndexByte(domain, '.')
	if idx == -1 {
		return domain + "a"
	}
	return domain[:idx] + "a" + domain[idx:]
}

func removeBenchmarkLetter(domain string) string {
	if domain == "" {
		return ""
	}
	idx := strings.IndexByte(domain, '.')
	if idx == -1 {
		if len(domain) <= 1 {
			return ""
		}
		return domain[:len(domain)-1]
	}
	if idx <= 1 {
		return ""
	}
	return domain[:idx-1] + domain[idx:]
}
