package main

import (
	"fmt"
	gos "github.com/starius/hipermap/gostaticdomainset"
)

func main() {
	seed := uint64(0x1122334455667788)
	inputs := []string{
		"", "com", "google", "google.com", "images", "images.google.com",
		"a", "A", "abc", "AbC", "xn--puny", "xn--punycode", "12345", "a.b", "zz.zz",
	}
	for _, s := range inputs {
		h := gos.Hash64SpanCI(s, seed)
		fmt.Printf("%q: 0x%016x\n", s, h)
	}
}
