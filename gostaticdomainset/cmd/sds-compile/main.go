package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"

	sds "github.com/starius/hipermap/gostaticdomainset"
)

func openInput(path string) (io.ReadCloser, error) {
	if path == "-" {
		return io.NopCloser(os.Stdin), nil
	}
	return os.Open(path)
}

func readPatterns(path string) ([]string, error) {
	r, err := openInput(path)
	if err != nil {
		return nil, err
	}
	defer r.Close()

	patterns := make([]string, 0, 1024)
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		s := strings.TrimSpace(sc.Text())
		if s == "" {
			continue
		}

		// Keep only the domain before first whitespace.
		if i := strings.IndexFunc(s, func(r rune) bool { return r == ' ' || r == '\t' }); i >= 0 {
			s = s[:i]
		}

		// Normalize by trimming trailing dots and lowercasing.
		for strings.HasSuffix(s, ".") {
			s = strings.TrimSuffix(s, ".")
		}

		if s == "" {
			continue
		}
		patterns = append(patterns, strings.ToLower(s))
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}

	return patterns, nil
}

func writeOutput(path string, data []byte) error {
	if path == "-" {
		_, err := os.Stdout.Write(data)
		return err
	}
	return os.WriteFile(path, data, 0o644)
}

func main() {
	inputPath := flag.String("input", "-", "path to domain patterns ('-' for stdin)")
	outputPath := flag.String("output", "-", "path to output DB ('-' for stdout)")
	flag.Parse()

	if flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "usage: sds-compile [-input path|-] [-output path|-]")
		os.Exit(2)
	}

	patterns, err := readPatterns(*inputPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "read patterns:", err)
		os.Exit(1)
	}

	ds, err := sds.Compile(patterns)
	if err != nil {
		fmt.Fprintln(os.Stderr, "compile DB:", err)
		os.Exit(1)
	}

	serialized, err := ds.Serialize()
	if err != nil {
		fmt.Fprintln(os.Stderr, "serialize DB:", err)
		os.Exit(1)
	}

	if err := writeOutput(*outputPath, serialized); err != nil {
		fmt.Fprintln(os.Stderr, "write output:", err)
		os.Exit(1)
	}
}
