package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"
	"strings"
)

// lessRevChar compares strings by reversed character order so that a base
// domain (e.g. "example.com") precedes its subdomains (e.g. "a.example.com").
func lessRevChar(a, b string) bool {
	ia, ib := len(a), len(b)
	for ia > 0 && ib > 0 {
		ca := a[ia-1]
		cb := b[ib-1]
		if ca != cb {
			return ca < cb
		}
		ia--
		ib--
	}
	return ia < ib
}

// hasSuffixOnLabelBoundary reports whether s ends with suf and either
// s == suf or the character before suf in s is a '.'.
func hasSuffixOnLabelBoundary(s, suf string) bool {
	if !strings.HasSuffix(s, suf) {
		return false
	}
	if len(s) == len(suf) {
		return true
	}
	// s ends with suf and is longer
	return s[len(s)-len(suf)-1] == '.'
}

// pruneSubdomains removes domains that are proper subdomains of a domain already kept.
func pruneSubdomains(domains []string) []string {
	if len(domains) == 0 {
		return domains
	}
	sort.Slice(domains, func(i, j int) bool { return lessRevChar(domains[i], domains[j]) })
	out := make([]string, 0, len(domains))
	for _, s := range domains {
		if len(out) == 0 {
			out = append(out, s)
			continue
		}
		base := out[len(out)-1]
		if s == base {
			continue
		}
		if hasSuffixOnLabelBoundary(s, base) {
			// s is a subdomain of base; skip
			continue
		}
		out = append(out, s)
	}
	return out
}

// countPopular counts suffixes (depth >= 2 labels) across domains and returns
// only those with counts > 16. A single domain contributes to all of its
// suffixes of depth >= 2, e.g., a.b.c contributes to b.c and a.b.c.
func countPopular(domains []string) map[string]int {
	counts := make(map[string]int)
	for _, d := range domains {
		if d == "" {
			continue
		}
		labels := strings.Split(d, ".")
		if len(labels) < 2 {
			continue
		}
		// Build all suffixes with depth >= 2
		for k := 2; k <= len(labels); k++ {
			suf := strings.Join(labels[len(labels)-k:], ".")
			counts[suf]++
		}
	}
	// Filter to >16 only
	for k, v := range counts {
		if v <= 16 || strings.Count(k, ".") < 1 { // ensure at least two components
			delete(counts, k)
		}
	}
	return counts
}

func main() {
	// Read domains from stdin, trim spaces and trailing dots, lowercase.
	scanner := bufio.NewScanner(os.Stdin)
	domains := make([]string, 0, 1024)
	seen := make(map[string]struct{}, 1024)
	for scanner.Scan() {
		s := strings.TrimSpace(scanner.Text())
		if s == "" {
			continue
		}
		// Trim trailing dots
		for strings.HasSuffix(s, ".") {
			s = strings.TrimSuffix(s, ".")
		}
		if s == "" {
			continue
		}
		s = strings.ToLower(s)
		if _, ok := seen[s]; ok {
			continue
		}
		seen[s] = struct{}{}
		domains = append(domains, s)
	}
	if err := scanner.Err(); err != nil {
		fmt.Fprintln(os.Stderr, "read error:", err)
		os.Exit(2)
	}

	// Prune subdomains
	domains = pruneSubdomains(domains)

	// Count popular suffixes
	counts := countPopular(domains)

	// Sort results by count desc, then lex asc
	type item struct {
		suf   string
		count int
	}
	items := make([]item, 0, len(counts))
	for k, v := range counts {
		items = append(items, item{suf: k, count: v})
	}
	sort.Slice(items, func(i, j int) bool {
		if items[i].count != items[j].count {
			return items[i].count > items[j].count
		}
		return items[i].suf < items[j].suf
	})

	// Print: "suffix\tcount"
	for _, it := range items {
		fmt.Printf("%s\t%d\n", it.suf, it.count)
	}
}
