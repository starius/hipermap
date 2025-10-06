package gostaticdomainset

import (
	"fmt"
	"strings"
)

// NaiveDomainSet is a simple, pure-Go implementation for testing.
// It lowercases inputs at build time and on queries checks all label-suffixes.
type NaiveDomainSet struct {
	m map[string]struct{}
}

// NewNaiveDomainSet builds a naive domain set with lowercase keys.
func NewNaiveDomainSet(domains []string) *NaiveDomainSet {
	m := make(map[string]struct{}, len(domains))
	for _, d := range domains {
		if d == "" {
			continue
		}
		m[strings.ToLower(d)] = struct{}{}
	}
	return &NaiveDomainSet{m: m}
}

// Find returns true if any whole-label suffix of domain is in the set.
func (n *NaiveDomainSet) Find(domain string) (bool, error) {
	if n == nil || len(n.m) == 0 {
		return false, nil
	}
	if domain == "" {
		return false, fmt.Errorf("empty domain")
	}
	// Trim trailing dots.
	for len(domain) > 0 && domain[len(domain)-1] == '.' {
		domain = domain[:len(domain)-1]
	}
	if domain == "" {
		return false, fmt.Errorf("empty domain after trim")
	}
	if len(domain) > 253 {
		return false, fmt.Errorf("domain too long: %d", len(domain))
	}
	if !isValidDomain(domain) {
		return false, fmt.Errorf("invalid domain characters")
	}
	s := strings.ToLower(domain)
	labels := strings.Split(s, ".")
	for i := 0; i < len(labels); i++ {
		suf := strings.Join(labels[i:], ".")
		if _, ok := n.m[suf]; ok {
			return true, nil
		}
	}
	return false, nil
}

// Pure-Go domain validation: ASCII-only [A-Za-z0-9-._]
func isValidDomain(s string) bool {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !((c >= 'A' && c <= 'Z') ||
			(c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') ||
			c == '-' || c == '.' || c == '_') {
			return false
		}
	}
	return true
}
