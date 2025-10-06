package main

import (
	"bufio"
	"flag"
	"fmt"
	"net/url"
	"os"
	"strings"
	"time"

	sds "github.com/starius/hipermap/gostaticdomainset"
)

func readLines(path string) ([]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var out []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		s := strings.TrimSpace(sc.Text())
		if s == "" {
			continue
		}
		// Patterns may include a space and a number after the domain.
		// Keep only the domain before first whitespace.
		if i := strings.IndexFunc(s, func(r rune) bool { return r == ' ' || r == '\t' }); i >= 0 {
			s = s[:i]
		}
		// Trim trailing dots and lowercase
		for strings.HasSuffix(s, ".") {
			s = strings.TrimSuffix(s, ".")
		}
		if s == "" {
			continue
		}
		out = append(out, strings.ToLower(s))
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func extractDomain(s string) (string, error) {
	// Cut by comma: "<url>,<count>"
	if i := strings.IndexByte(s, ','); i >= 0 {
		s = s[:i]
	}
	s = strings.TrimSpace(s)
	if s == "" {
		return "", fmt.Errorf("empty URL field")
	}
	// Ensure scheme for url.Parse
	raw := s
	if !strings.Contains(raw, "://") {
		raw = "http://" + raw
	}
	u, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("parse url: %w", err)
	}
	h := u.Host
	if h == "" {
		return "", fmt.Errorf("empty host in url")
	}
	// Strip port if any
	if i := strings.IndexByte(h, ':'); i >= 0 {
		h = h[:i]
	}
	// Lowercase + trim trailing dots
	h = strings.ToLower(strings.TrimRight(h, "."))
	if h == "" {
		return "", fmt.Errorf("empty host after trim")
	}
	return h, nil
}

func main() {
	patternsPath := flag.String("patterns", "", "path to patterns file (one domain per line)")
	textPath := flag.String("text", "", "path to text file with 'url,count' lines")
	flag.Parse()

	if *patternsPath == "" || *textPath == "" {
		fmt.Fprintln(os.Stderr, "usage: verify -patterns=patterns.txt -text=text.csv")
		os.Exit(2)
	}

	patterns, err := readLines(*patternsPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "read patterns:", err)
		os.Exit(1)
	}
	if len(patterns) == 0 {
		fmt.Fprintln(os.Stderr, "no patterns loaded")
		os.Exit(1)
	}

	// Build optimized DB
	ds, err := sds.Compile(patterns)
	if err != nil {
		fmt.Fprintln(os.Stderr, "compile optimized DB:", err)
		os.Exit(1)
	}
	// Print DB summary immediately after compile and its serialized size
	fmt.Println(ds.String())
	if ser, err := ds.Serialize(); err == nil {
		fmt.Printf("Serialized size: %d\n", len(ser))
	} else {
		fmt.Fprintln(os.Stderr, "serialize error:", err)
		os.Exit(1)
	}
	// Build a deserialized copy from serialized bytes
	ser, err := ds.Serialize()
	if err != nil {
		fmt.Fprintln(os.Stderr, "serialize error:", err)
		os.Exit(1)
	}
	ds2, err := sds.FromSerialized(ser)
	if err != nil {
		fmt.Fprintln(os.Stderr, "deserialize copy error:", err)
		os.Exit(1)
	}

	// Build naive DB
	naive := sds.NewNaiveDomainSet(patterns)

	// Validate all pattern domains against both implementations and
	// also some derivatives: add subdomain, remove subdomain, add letter,
	// remove letter (at the beginning in all cases).
	// Do not fail fast on mismatches; log and continue to provide a summary,
	// and print per-label totals.
	var patternMismatches int
	var patternErrors int
	labels := []string{"exact", "add_subdomain", "remove_subdomain", "add_letter", "remove_letter", "trailing_dot"}
	patternTotals := map[string]int{"exact": 0, "add_subdomain": 0, "remove_subdomain": 0, "add_letter": 0, "remove_letter": 0, "trailing_dot": 0}
	patternMism := map[string]int{"exact": 0, "add_subdomain": 0, "remove_subdomain": 0, "add_letter": 0, "remove_letter": 0, "trailing_dot": 0}
	patternErrs := map[string]int{"exact": 0, "add_subdomain": 0, "remove_subdomain": 0, "add_letter": 0, "remove_letter": 0, "trailing_dot": 0}
	for idx, p := range patterns {
		// Helper to check a single domain
		which := 0
		check := func(label, dom string) {
			patternTotals[label]++
			// Alternate between original and deserialized DB for coverage
			var gotFast bool
			var errFast error
			if which%2 == 0 {
				gotFast, errFast = ds.Find(dom)
			} else {
				gotFast, errFast = ds2.Find(dom)
			}
			which++
			if errFast != nil {
				fmt.Fprintf(os.Stderr, "error on pattern[%d] %s: domain=%q err=%v\n", idx, label, dom, errFast)
				patternErrs[label]++
				patternErrors++
				return
			}
			gotNaive, _ := naive.Find(dom)
			if gotFast != gotNaive {
				fmt.Fprintf(os.Stderr, "mismatch on pattern[%d] %s: domain=%q fast=%v naive=%v\n", idx, label, dom, gotFast, gotNaive)
				patternMismatches++
				patternMism[label]++
			}
		}
		// Exact
		check("exact", p)
		// Add subdomain; skip if resulting domain exceeds 253 bytes.
		if d := "x." + p; len(d) <= 253 {
			check("add_subdomain", d)
		}
		// Remove subdomain (drop leftmost label if present)
		if i := strings.IndexByte(p, '.'); i >= 0 && i+1 < len(p) {
			check("remove_subdomain", p[i+1:])
		}
		// Add letter at the beginning; skip if resulting domain exceeds 253 bytes.
		if d := "a" + p; len(d) <= 253 {
			check("add_letter", d)
		}
		// Remove letter at the beginning if non-empty
		if len(p) > 0 {
			check("remove_letter", p[1:])
		}
		// Trailing dot variant
		check("trailing_dot", p+".")
	}

	f, err := os.Open(*textPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "open text:", err)
		os.Exit(1)
	}
	defer f.Close()

	sc := bufio.NewScanner(f)
	total := 0 // total lines read
	valid := 0 // successfully parsed domains
	fastMatched := 0
	naiveMatched := 0
	discrepancies := 0
	parseErrors := 0
	fastFindErrors := 0
	naiveFindErrors := 0
	var fastN, naiveN int
	var fastTotal time.Duration
	var naiveTotal time.Duration
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		total++
		dom, err := extractDomain(line)
		if err != nil {
			fmt.Fprintf(os.Stderr, "parse error on line %d: %v\n", total, err)
			parseErrors++
			continue
		}
		valid++
		// Alternate between original (odd lines) and deserialized (even lines)
		useDs2 := (total%2 == 0)
		t0 := time.Now()
		var gotFast bool
		var errFast error
		if useDs2 {
			gotFast, errFast = ds2.Find(dom)
		} else {
			gotFast, errFast = ds.Find(dom)
		}
		fastTotal += time.Since(t0)
		fastN++
		if errFast != nil {
			fmt.Fprintf(os.Stderr, "find error (fast) on line %d: url=%q domain=%q err=%v\n", total, line, dom, errFast)
			fastFindErrors++
			continue
		}
		t1 := time.Now()
		gotNaive, errNaive := naive.Find(dom)
		naiveTotal += time.Since(t1)
		naiveN++
		if errNaive != nil {
			fmt.Fprintf(os.Stderr, "find error (naive) on line %d: url=%q domain=%q err=%v\n", total, line, dom, errNaive)
			naiveFindErrors++
			// Count as discrepancy and continue; fast matched count already updated if applicable.
			// Do not increment naiveMatched.
			// Note: also count as discrepancy below if gotFast differs from false (implicit).
		}

		if errNaive != nil || gotFast != gotNaive {
			discrepancies++
			fmt.Fprintf(os.Stderr, "mismatch on line %d: url=%q domain=%q fast=%v naive=%v err_naive=%v\n", total, line, dom, gotFast, gotNaive, errNaive)
		}
		if gotFast {
			fastMatched++
		}
		if errNaive == nil && gotNaive {
			naiveMatched++
		}
	}
	if err := sc.Err(); err != nil {
		fmt.Fprintln(os.Stderr, "read text:", err)
		os.Exit(1)
	}

	fmt.Println(ds.String())
	if ser, err := ds.Serialize(); err == nil {
		fmt.Printf("Serialized size: %d\n", len(ser))
	} else {
		fmt.Fprintln(os.Stderr, "serialize error:", err)
		os.Exit(1)
	}
	// Summary
	var fastPct, naivePct, discPct float64
	if valid > 0 {
		fastPct = float64(fastMatched) * 100.0 / float64(valid)
		naivePct = float64(naiveMatched) * 100.0 / float64(valid)
		discPct = float64(discrepancies) * 100.0 / float64(valid)
	}
	fmt.Printf("Inputs: total=%d, valid=%d, parse_errors=%d\n", total, valid, parseErrors)
	fmt.Printf("Fast matches:  %d of %d (%.3f%%)\n", fastMatched, valid, fastPct)
	fmt.Printf("Naive matches: %d of %d (%.3f%%)\n", naiveMatched, valid, naivePct)
	// Per-label pattern validation totals
	fmt.Printf("Pattern checks by label:\n")
	for _, lbl := range labels {
		fmt.Printf("  %-16s tests=%d mismatches=%d errors=%d\n", lbl+":", patternTotals[lbl], patternMism[lbl], patternErrs[lbl])
	}
	fmt.Printf("Pattern check mismatches: %d errors: %d\n", patternMismatches, patternErrors)
	if valid > 0 {
		fmt.Printf("Discrepancies: %d of %d (%.3f%%)\n", discrepancies, valid, discPct)
	} else {
		fmt.Printf("Discrepancies: %d of %d (%.3f%%)\n", discrepancies, valid, 0.0)
	}
	if total > 0 {
		errTotal := parseErrors + fastFindErrors + naiveFindErrors
		errPct := float64(errTotal) * 100.0 / float64(total)
		fmt.Printf("Text errors: parse=%d fast_find=%d naive_find=%d total=%d (%.3f%% of lines)\n", parseErrors, fastFindErrors, naiveFindErrors, errTotal, errPct)
	} else {
		fmt.Printf("Text errors: parse=%d fast_find=%d naive_find=%d total=%d (0.000%% of lines)\n", parseErrors, fastFindErrors, naiveFindErrors, parseErrors+fastFindErrors+naiveFindErrors)
	}

	// Average find latencies (ns/op). Use independent call counts to avoid division by zero.
	if fastN > 0 {
		fmt.Printf("Avg find latency (fast):  %.0f ns\n", float64(fastTotal.Nanoseconds())/float64(fastN))
	}
	if naiveN > 0 {
		fmt.Printf("Avg find latency (naive): %.0f ns\n", float64(naiveTotal.Nanoseconds())/float64(naiveN))
	}
}
