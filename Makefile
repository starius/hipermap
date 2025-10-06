SHELL := /bin/sh

.PHONY: build test fmt fmt-c fmt-go fmt-nix \
	nix nix-musl nix-glibc nix-debug nix-sanitizers nix-avx512 nix-arm64 nix-mingw64 nix-nosimd

# Configurable locations
BUILD_DIR ?= build
PREFIX ?= $(BUILD_DIR)/out
ABS_PREFIX := $(abspath $(PREFIX))
CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release

# CGO flags to link Go tools against the locally built C library
CGO_CFLAGS ?= -I$(ABS_PREFIX)/include
CGO_LDFLAGS ?= -L$(ABS_PREFIX)/lib -lhipermap -lstdc++ -pthread
GO_TESTFLAGS ?=

# Auto-detect sanitizer builds from CMake cache (non-Nix local builds)
SAN_CACHE := $(BUILD_DIR)/CMakeCache.txt
CM_SAN := $(shell [ -f "$(SAN_CACHE)" ] && grep -q '^HIPERMAP_ENABLE_SANITIZERS:BOOL=ON' "$(SAN_CACHE)" && echo 1 || echo 0)
# User can force via SANITIZERS=1; otherwise follow CMake setting
SAN_ENABLED := $(if $(filter 1,$(SANITIZERS)),1,$(CM_SAN))

ifeq ($(SAN_ENABLED),1)
CGO_CFLAGS += -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
CGO_LDFLAGS += -fsanitize=address -fsanitize=undefined -ldl -lubsan -lm
endif

# Build everything without Nix: CMake library/tools + Go packages/tools
build:
	@echo "==> Configure CMake in $(BUILD_DIR)"
	cmake -B $(BUILD_DIR) -S . $(CMAKE_FLAGS)
	@echo "==> Build C/C++ targets"
	cmake --build $(BUILD_DIR) --parallel
	@echo "==> Install C/C++ artifacts to $(PREFIX)"
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)
	@echo "==> Build Go packages"
	CGO_ENABLED=1 CGO_CFLAGS="$(CGO_CFLAGS)" CGO_LDFLAGS="$(CGO_LDFLAGS)" go build ./...
	@echo "==> Build Go tools"
	CGO_ENABLED=1 CGO_CFLAGS="$(CGO_CFLAGS)" CGO_LDFLAGS="$(CGO_LDFLAGS)" \
		go build -trimpath -buildvcs=false -o $(PREFIX)/bin/verify ./gostaticdomainset/cmd/verify
	@echo "Build complete. Binaries in $(PREFIX)/bin, install tree at $(PREFIX)."

# Run all Go tests in all subpackages
test:
	@echo "==> Running Go tests"
	CGO_ENABLED=1 CGO_CFLAGS="$(CGO_CFLAGS)" CGO_LDFLAGS="$(CGO_LDFLAGS)" go test ./... $(GO_TESTFLAGS)

pure-test:
	@echo "==> Running tests of pure Go static domain set implementation"
	CGO_ENABLED=0 go test ./gostaticdomainset -tags use_pure_gostaticdomainset $(GO_TESTFLAGS)
	CGO_ENABLED=1 CGO_CFLAGS="$(CGO_CFLAGS)" CGO_LDFLAGS="$(CGO_LDFLAGS)" go test ./gostaticdomainset -run TestPureCGOSerializationCompatibility $(GO_TESTFLAGS)

# Format everything: C/C++ sources, Go code, and flake.nix.
fmt: fmt-c fmt-go fmt-nix

# Format C/C++ sources and headers.
fmt-c:
	clang-format -i *.h *.c *.cpp tools/*.c tools/*.cpp

# Format all Go packages in this module.
fmt-go:
	go fmt ./...

# Format flake.nix with nixfmt (RFC style).
fmt-nix:
	nixfmt flake.nix

# -----------------------
# Nix flake convenience targets
# -----------------------

NIX ?= nix
NIX_ARGS ?= -L

define nix_build
	@echo "==> nix build .#$(1) -> out-$(2)"
	$(NIX) build $(NIX_ARGS) .#$(1) -o out-$(2)
endef

nix-musl:
	$(call nix_build,musl,musl)

nix-glibc:
	$(call nix_build,glibc,glibc)

nix-debug:
	$(call nix_build,debug,debug)

nix-sanitizers:
	$(call nix_build,sanitizers,sanitizers)

nix-avx512:
	$(call nix_build,avx512,avx512)

nix-arm64:
	$(call nix_build,arm64,arm64)


nix-mingw64:
	$(call nix_build,mingw64,mingw64)

nix-nosimd:
	$(call nix_build,nosimd,nosimd)

# Meta target: build all nix outputs, each in its own out-XXX symlink
nix: nix-musl nix-glibc nix-debug nix-sanitizers nix-avx512 nix-arm64 nix-mingw64 nix-nosimd
