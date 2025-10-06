package gostaticdomainset

import (
	"testing"
)

// FuzzStaticDomainSet_Find checks runtime safety and parity (when valid) with the naive matcher.
// Run via: go test -run ^$ -fuzz=Fuzz -fuzztime=60s ./gostaticdomainset
func FuzzStaticDomainSet_Find(f *testing.F) {
	// Prepare a diverse baseline of domains to build the DB once.
	base := []string{
		"example.com",
		"images.google.com",
		"a..b.com",
		"-start.com",
		"end-.com",
		"mi-d.le-.ex-ample.com",
		"xn--puny-test.com",
	}
	// Add some long labels
	base = append(base,
		makeLabelN(63),
		makeLabelN(64),
		makeLabelN(120),
		makeLabelN(200),
	)

	ds, err := Compile(base)
	if err != nil {
		// If we cannot build the baseline, bail out early.
		f.Fatalf("failed to compile baseline: %v", err)
	}
	naive := NewNaiveDomainSet(base)

	// Seed inputs that exercise a range of cases
	seeds := []string{
		"example.com",
		"api.example.com",
		"example.com.",
		"..example.com...",
		"images.google.com",
		"x.images.google.com",
		"a..b.com",
		"x.a..b.com",
		"-start.com",
		"end-.com",
		string([]byte{0x7f, 'a', '.', 'c', 'o', 'm'}), // DEL + ASCII
		"white space.com", // invalid
	}
	for _, s := range seeds {
		f.Add(s)
	}

	f.Fuzz(func(t *testing.T, s string) {
		// Constrain length and bytes to ASCII range
		if len(s) > 512 {
			s = s[:512]
		}
		// Map to 7-bit ASCII to avoid multi-byte runes in C
		b := []byte(s)
		for i := range b {
			b[i] &= 0x7F
		}
		s = string(b)

		got, err1 := ds.Find(s)
		want, err2 := naive.Find(s)

		// If either side reports an error, we only assert that the C path did not crash.
		if err1 != nil || err2 != nil {
			return
		}
		if got != want {
			t.Fatalf("parity mismatch for %q: got=%v want=%v", s, got, want)
		}
	})
}
