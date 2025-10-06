package main

import (
	"flag"
	"fmt"
	"math/rand"
	"os"
	"strings"
	"time"

	sds "github.com/starius/hipermap/gostaticdomainset"
)

// makeGroup returns N subdomains of base like x0.base, x1.base, ...
func makeGroup(base string, n int) []string {
	out := make([]string, 0, n)
	for i := 0; i < n; i++ {
		out = append(out, fmt.Sprintf("x%d.%s", i, base))
	}
	return out
}

func main() {
	base := flag.String("base", "popular.example.com", "base suffix to make popular (do not include in patterns)")
	extra := flag.String("extra", "tld", "unrelated base suffix to try collisions against (last two labels)")
	n := flag.Int("n", 40, "number of subdomains to generate for base")
	maxTries := flag.Int("tries", 200000, "max candidates to try for collision")
	brute := flag.Bool("bruteforce", true, "enable brute-force placement-mismatch search (slow)")
	flag.Parse()

	// Build initial patterns: many subdomains of base (make it popular), do not include the base itself.
	var patterns []string
	patterns = append(patterns, makeGroup(*base, *n)...)

	ds, err := sds.Compile(patterns)
	if err != nil {
		fmt.Fprintln(os.Stderr, "compile error:", err)
		os.Exit(1)
	}
	fmt.Println(ds.String())

	seed := ds.Seed()
	target := sds.Hash16SuffixCI(*base, seed)

	if !*brute {
		// Deterministic 16-bit collision hunt by hash
		var candidateSuffix string
		var found bool
		for i := 0; i < *maxTries; i++ {
			suffix := fmt.Sprintf("c%d.%s", i, *extra)
			if sds.Hash16SuffixCI(suffix, seed) == target {
				candidateSuffix = suffix
				found = true
				break
			}
		}
		if !found {
			fmt.Fprintf(os.Stderr, "no 16-bit collision found within %d tries (seed=0x%x target=0x%04x)\n", *maxTries, seed, target)
			os.Exit(2)
		}
		candidate := candidateSuffix // two labels
		query := "a." + candidate    // subdomain triggers popular growth
		fmt.Printf("Found candidate with colliding 16-bit popular hash: %q, hash=0x%04x\n", candidate, target)

		patterns2 := append(append([]string(nil), patterns...), candidate)
		ds2, err := sds.Compile(patterns2)
		if err != nil {
			fmt.Fprintln(os.Stderr, "compile error (with candidate):", err)
			os.Exit(1)
		}
		fmt.Println(ds2.String())
		naive := sds.NewNaiveDomainSet(patterns2)
		gotFast, errFast := ds2.Find(query)
		if errFast != nil {
			fmt.Fprintln(os.Stderr, "find error (fast):", errFast)
			os.Exit(1)
		}
		gotNaive, errN := naive.Find(query)
		if errN != nil {
			fmt.Fprintf(os.Stderr, "naive find error for %q: %v\n", query, errN)
			os.Exit(1)
		}
		if gotFast != gotNaive {
			fmt.Printf("Reproduced mismatch: fast=%v naive=%v for query=%q (stored=%q seed=0x%x target=0x%04x)\n", gotFast, gotNaive, query, candidate, seed, target)
			os.Exit(0)
		}
		fmt.Printf("No mismatch observed: fast=%v naive=%v for query=%q (stored=%q seed=0x%x target=0x%04x)\n", gotFast, gotNaive, query, candidate, seed, target)
		os.Exit(3)
	}

	// Brute-force: try random unrelated two-label domains under extra, add and query its subdomain.
	rnd := rand.New(rand.NewSource(time.Now().UnixNano()))
	const letters = "abcdefghijklmnopqrstuvwxyz0123456789"
	genLabel := func(min, max int) string {
		ln := rnd.Intn(max-min+1) + min
		var b strings.Builder
		b.Grow(ln)
		for i := 0; i < ln; i++ {
			b.WriteByte(letters[rnd.Intn(len(letters))])
		}
		return b.String()
	}

	for i := 0; i < *maxTries; i++ {
		candidate := fmt.Sprintf("%s.%s", genLabel(3, 8), *extra) // two labels
		query := "a." + candidate                                 // not present, should match via suffix
		patterns2 := append(append([]string(nil), patterns...), candidate)
		ds2, err := sds.Compile(patterns2)
		if err != nil {
			fmt.Fprintln(os.Stderr, "compile error (with candidate):", err)
			os.Exit(1)
		}
		naive := sds.NewNaiveDomainSet(patterns2)
		fast, err := ds2.Find(query)
		if err != nil {
			fmt.Fprintln(os.Stderr, "find error (fast):", err)
			os.Exit(1)
		}
		ref, errN := naive.Find(query)
		if errN != nil {
			fmt.Fprintf(os.Stderr, "naive find error for %q: %v\n", query, errN)
			os.Exit(1)
		}
		if fast != ref {
			fmt.Printf("Reproduced mismatch via brute force after %d tries: fast=%v naive=%v for query=%q (stored=%q seed=0x%x)\n", i+1, fast, ref, query, candidate, seed)
			fmt.Println(ds2.String())
			os.Exit(0)
		}
		if (i+1)%100 == 0 {
			fmt.Printf("... tried %d, no mismatch yet\n", i+1)
		}
	}
	fmt.Printf("No mismatch observed in %d tries (seed=0x%x)\n", *maxTries, seed)
	os.Exit(3)
}
