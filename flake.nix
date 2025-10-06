{
  description = "Hipermap: C/C++ lib + bench tool, Go verify tool";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.flake-utils.url = "github:numtide/flake-utils";
  # Shadowsocks ipset source (non-flake). Using an input avoids hardcoding a
  # commit here; the lock file will pin it.
  inputs.ipset-ss = {
    url = "github:shadowsocks/ipset";
    flake = false;
  };

  outputs =
    inputs@{
      self,
      nixpkgs,
      flake-utils,
      ipset-ss,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        pkgsMusl = pkgs.pkgsMusl;

        # Build the shadowsocks/ipset library used by static_map_benchmark
        ipsetLib = pkgs.stdenv.mkDerivation rec {
          pname = "ipset-ss";
          version = "unstable";
          src = ipset-ss;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.pkg-config
            pkgs.git
          ];
          buildInputs = [ pkgs.libcork ];
          # match install-deps.sh: drop -Werror, set pthread
          postPatch = ''
            substituteInPlace CMakeLists.txt --replace "add_definitions(-Wall -Werror)" ""
            # Disable building tests and docs (avoid dependency on 'check' and pandoc)
            substituteInPlace CMakeLists.txt --replace "add_subdirectory(tests)" "# disabled by nix"
            substituteInPlace CMakeLists.txt --replace "add_subdirectory(docs)" "# disabled by nix"
          '';
          cmakeFlags = [
            "-DCMAKE_INSTALL_PREFIX=$out"
            "-DCMAKE_C_FLAGS=-pthread"
          ];
          installPhase = ''
            runHook preInstall
            cmake --install . --prefix "$out"
            runHook postInstall
          '';
        };

        # CMake builder.
        mkCMake =
          {
            pkgsX,
            stdenvX,
            buildType ? "Release",
            enableSanitizers ? false,
            enableAvx512 ? false,
            disableSimd ? false,
            domainBenchEnableHyperscan ? true,
            enableStaticMapBenchmark ? false,
            staticLink ? false,
          }:
          let
            cmakeFlagsList = [
              "-DCMAKE_BUILD_TYPE=${buildType}"
              "-DHIPERMAP_ENABLE_SANITIZERS=${if enableSanitizers then "ON" else "OFF"}"
              "-DHIPERMAP_ENABLE_AVX512=${if enableAvx512 then "ON" else "OFF"}"
              "-DHIPERMAP_DOMAIN_BENCH_ENABLE_HYPERSCAN=${if domainBenchEnableHyperscan then "ON" else "OFF"}"
              "-DHIPERMAP_DISABLE_SIMD=${if disableSimd then "ON" else "OFF"}"
              "-DHIPERMAP_ENABLE_STATIC_MAP_BENCHMARK=${if enableStaticMapBenchmark then "ON" else "OFF"}"
            ]
            ++ (
              if staticLink then
                [
                  "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libstdc++ -static-libgcc"
                  "-DHIPERMAP_FULLY_STATIC=ON"
                  "-DBUILD_SHARED_LIBS=OFF"
                  "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
                ]
              else
                [ ]
            );
            cmakeFlagsStr = pkgs.lib.escapeShellArgs cmakeFlagsList;
            hsInputs =
              if enableStaticMapBenchmark || domainBenchEnableHyperscan then
                [
                  (pkgsX.hyperscan.override { withStatic = true; })
                  pkgsX.hyperscan.dev
                ]
              else
                [ ];
            benchDeps =
              if enableStaticMapBenchmark then
                [
                  ipsetLib
                  pkgsX.libcork
                ]
              else
                [ ];
            extraTargets = pkgs.lib.optionalString (enableStaticMapBenchmark) " static_map_benchmark";
          in
          stdenvX.mkDerivation {
            pname = "hipermap";
            version = "1.0";
            src = self;
            strictDeps = true;
            nativeBuildInputs = [
              pkgs.cmake
              pkgs.pkg-config
            ];
            buildInputs = hsInputs ++ benchDeps;
            # Disable PIE hardening for fully static builds to avoid ET_DYN/INTERP
            hardeningDisable = pkgs.lib.optionals staticLink [ "pie" ];
            configurePhase = ''
              runHook preConfigure
              cmake -B build -S . ${cmakeFlagsStr}
              runHook postConfigure
            '';
            buildPhase = ''
              runHook preBuild
              cmake --build build --target hipermap bench_domains bench_lower test_cache${extraTargets}
              runHook postBuild
            '';
            installPhase = (
              if (stdenvX.hostPlatform.isWindows or false) then
                ''
                  runHook preInstall
                  mkdir -p "$out/bin" "$out/lib" "$out/include/hipermap"
                  # Library
                  if [ -f build/libhipermap.a ]; then
                    cp -f build/libhipermap.a "$out/lib/"
                  else
                    cp -f build/CMakeFiles/hipermap.dir/../libhipermap.a "$out/lib/" 2>/dev/null || true
                  fi
                  # Headers
                  cp -f common.h static_map.h cache.h static_uint64_set.h static_uint64_map.h static_domain_set.h "$out/include/hipermap/"
                  # Executables
                  for exe in bench_domains bench_lower static_map_benchmark test_cache; do
                    name="$exe"
                    src=""
                    if [ -f "build/''${name}.exe" ]; then
                      src="build/''${name}.exe"
                    elif [ -f "build/''${name}" ]; then
                      src="build/''${name}"
                    fi
                    if [ -n "$src" ]; then
                      dst="$out/bin/''${name}.exe"
                      cp -f "$src" "$dst" || true
                    fi
                  done
                  runHook postInstall
                ''
              else
                ''
                  runHook preInstall
                  cmake --install build --prefix "$out"
                  runHook postInstall
                ''
            );
          };

        # Common Go module builder (verify or tests) with cgo and external linker
        mkGoModuleCommon =
          {
            pkgsX,
            stdenvX,
            hipermapDrv,
            pname,
            subPackages,
            buildTestBinaries ? false,
            staticLink ? false,
            enableSanitizers ? false,
            goos ? null,
            goarch ? null,
          }:
          pkgs.buildGoModule rec {
            inherit pname subPackages buildTestBinaries;
            version = "1.0";
            src = self;
            modRoot = ".";
            vendorHash = "sha256-sWZeLYTQ22aRlBBrpGlMzvJ+ySgr+tVBQT7bmHIPHtE=";
            nativeBuildInputs = [ pkgsX.pkg-config ];
            buildInputs = [
              stdenvX.cc.cc
            ]
            ++ (pkgs.lib.optionals (stdenvX.hostPlatform.isMusl or false) [ pkgsX.musl ]);
            CC = "${stdenvX.cc}/bin/${stdenvX.cc.targetPrefix}cc";
            hardeningDisable = [ "fortify" ];
            CGO_CFLAGS = "-I${hipermapDrv}/include";
            CGO_LDFLAGS =
              let
                isMusl = (stdenvX.hostPlatform.isMusl or false);
                isWindows = (stdenvX.hostPlatform.isWindows or false);
                pthread = if (isMusl || isWindows) then "" else " -pthread";
                base = "-L${hipermapDrv}/lib -lhipermap -lstdc++" + pthread;
                san = (
                  if enableSanitizers then " -fsanitize=address -fsanitize=undefined -ldl -lubsan -lm" else ""
                );
                stat = (if staticLink then " -static" else "");
              in
              base + san + stat;
            ldflags = pkgs.lib.optionals staticLink [
              "-linkmode=external"
              "-extld=${stdenvX.cc}/bin/${stdenvX.cc.targetPrefix}cc"
              "-extldflags=-static"
            ];
          };

        # Go verify binary
        mkGoVerify =
          args:
          mkGoModuleCommon (
            args
            // {
              pname = "verify";
              subPackages = [ "gostaticdomainset/cmd/verify" ];
              buildTestBinaries = false;
            }
          );

        # Go test binaries for all packages
        mkGoTests =
          args:
          mkGoModuleCommon (
            args
            // {
              pname = "hipermap-tests";
              subPackages = [ "./..." ];
              buildTestBinaries = true;
            }
          );

      in
      {
        # Enable `nix fmt` by providing a formatter for this system.
        formatter = pkgs.nixfmt-rfc-style;

        packages = {
          # Default: fully static (musl), Hyperscan disabled
          musl =
            let
              # On musl we avoid Hyperscan/ipset/cork to keep cache use high
              hip = mkCMake {
                pkgsX = pkgsMusl;
                stdenvX = pkgsMusl.stdenv;
                domainBenchEnableHyperscan = false;
                enableStaticMapBenchmark = false;
                staticLink = true;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-all-musl";
              paths = [
                hip
                (mkGoVerify {
                  pkgsX = pkgsMusl;
                  stdenvX = pkgsMusl.stdenv;
                  hipermapDrv = hip;
                  staticLink = true;
                })
                (mkGoTests {
                  pkgsX = pkgsMusl;
                  stdenvX = pkgsMusl.stdenv;
                  hipermapDrv = hip;
                  staticLink = true;
                })
              ];
            };

          # glibc bundle: CMake build (Hyperscan ON) + Go verify (no Hyperscan dependency)
          glibc =
            let
              hip = mkCMake {
                pkgsX = pkgs;
                stdenvX = pkgs.stdenv;
                domainBenchEnableHyperscan = true;
                enableStaticMapBenchmark = true;
                staticLink = false;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-glibc";
              paths = [
                hip
                (mkGoVerify {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                  staticLink = false;
                })
                (mkGoTests {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                  staticLink = false;
                })
              ];
            };

          # Debug/sanitizer/AVX512 variants for glibc
          debug =
            let
              hip = mkCMake {
                pkgsX = pkgs;
                stdenvX = pkgs.stdenv;
                buildType = "Debug";
                domainBenchEnableHyperscan = true;
                enableStaticMapBenchmark = true;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-debug";
              paths = [
                hip
                (mkGoVerify {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                })
                (mkGoTests {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                })
              ];
            };

          sanitizers =
            let
              hip = mkCMake {
                pkgsX = pkgs;
                stdenvX = pkgs.stdenv;
                enableSanitizers = true;
                domainBenchEnableHyperscan = true;
                enableStaticMapBenchmark = true;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-sanitizers";
              paths = [
                hip
                (mkGoVerify {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                  enableSanitizers = true;
                })
                (mkGoTests {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                  enableSanitizers = true;
                })
              ];
            };

          avx512 =
            let
              hip = mkCMake {
                pkgsX = pkgs;
                stdenvX = pkgs.stdenv;
                enableAvx512 = true;
                domainBenchEnableHyperscan = true;
                enableStaticMapBenchmark = true;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-avx512";
              paths = [
                hip
                (mkGoVerify {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                })
                (mkGoTests {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                })
              ];
            };

          nosimd =
            let
              hip = mkCMake {
                pkgsX = pkgs;
                stdenvX = pkgs.stdenv;
                enableSanitizers = true;
                domainBenchEnableHyperscan = true;
                enableStaticMapBenchmark = true;
                disableSimd = true;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-nosimd";
              paths = [
                hip
                (mkGoVerify {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                  enableSanitizers = true;
                })
                (mkGoTests {
                  pkgsX = pkgs;
                  stdenvX = pkgs.stdenv;
                  hipermapDrv = hip;
                  enableSanitizers = true;
                })
              ];
            };

          # Cross-built bundle for ARM64 (aarch64-linux), Hyperscan OFF
          arm64 =
            let
              # Use musl cross toolchain to enable fully static linking
              cross = pkgs.pkgsCross.aarch64-multiplatform-musl;
              hip = mkCMake {
                pkgsX = cross;
                stdenvX = cross.stdenv;
                domainBenchEnableHyperscan = false;
                enableStaticMapBenchmark = false;
                staticLink = true;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-arm64";
              paths = [ hip ];
            };

          # Cross-built bundle for Windows x86_64 (mingw64), Hyperscan OFF
          mingw64 =
            let
              cross = pkgs.pkgsCross.mingwW64;
              hip = mkCMake {
                pkgsX = cross;
                stdenvX = cross.stdenv;
                domainBenchEnableHyperscan = false;
                enableStaticMapBenchmark = false;
                staticLink = true;
              };
            in
            pkgs.symlinkJoin {
              name = "hipermap-mingw64";
              paths = [ hip ];
            };

          default = self.packages.${system}.musl;
        };

        devShells.default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.go
            pkgs.pkg-config
          ];
          buildInputs = [
            (pkgs.hyperscan.override { withStatic = true; })
            pkgs.hyperscan.dev
            pkgs.libcork
            ipsetLib
          ];
        };
      }
    );
}
