SHELL := /bin/sh

.PHONY: fmt fmt-c fmt-go

# Format everything: C/C++ sources and Go code.
fmt: fmt-c fmt-go

# Format C/C++ sources and headers.
fmt-c:
	clang-format -i *.h *.c *.cpp tools/*.c

# Format all Go packages in this module.
fmt-go:
	go fmt ./...
