SHELL := /bin/sh

.PHONY: build test fmt fmt-c fmt-go

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

# Build everything: CMake library/tools + Go packages/tools
build:
	@echo "==> Configure CMake in $(BUILD_DIR)"
	cmake -B $(BUILD_DIR) -S . $(CMAKE_FLAGS)
	@echo "==> Build C/C++ targets"
	cmake --build $(BUILD_DIR) --parallel
	@echo "==> Install C/C++ artifacts to $(PREFIX)"
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)
	@echo "==> Build Go packages"
	CGO_ENABLED=1 CGO_CFLAGS="$(CGO_CFLAGS)" CGO_LDFLAGS="$(CGO_LDFLAGS)" go build ./...
	@echo "Build complete. Binaries in $(PREFIX)/bin, install tree at $(PREFIX)."

# Run all Go tests in all subpackages
test:
	@echo "==> Running Go tests"
	CGO_ENABLED=1 CGO_CFLAGS="$(CGO_CFLAGS)" CGO_LDFLAGS="$(CGO_LDFLAGS)" go test ./... $(GO_TESTFLAGS)

# Format everything: C/C++ sources and Go code.
fmt: fmt-c fmt-go

# Format C/C++ sources and headers.
fmt-c:
	clang-format -i *.h *.c *.cpp tools/*.c

# Format all Go packages in this module.
fmt-go:
	go fmt ./...
