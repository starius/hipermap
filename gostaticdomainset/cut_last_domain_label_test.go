package gostaticdomainset

import (
	"testing"

	"github.com/stretchr/testify/require"
)

func TestCutLastDomainLabelOffset(t *testing.T) {
	cases := []struct {
		in   string
		want int
	}{
		{"", 0},
		{"com", 0},
		{"google.com", 7},
		{"a.b.c", 4},
		{"a.b.c.", 6},
		{"a", 0},
		{"a.", 2},
		{".com", 1},
		{"..com", 2},
		{"abc.def.ghi.jkl", len("abc.def.ghi.")},
	}
	for _, tc := range cases {
		got := CutLastDomainLabelOffset(tc.in)
		require.Equal(t, tc.want, got, "input=%q", tc.in)
	}
}
