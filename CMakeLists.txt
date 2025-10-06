cmake_minimum_required(VERSION 3.18.4)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
        FORCE)
endif(NOT CMAKE_BUILD_TYPE)

project(
  hipermap
  VERSION 1.0
  LANGUAGES C CXX
)

# Option to force fully static executables (no PT_INTERP)
option(HIPERMAP_FULLY_STATIC "Link executables fully static (-static -static-libstdc++ -static-libgcc)" OFF)

if (HIPERMAP_FULLY_STATIC AND NOT MSVC)
  # Ensure global linker flags request full static linking
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libstdc++ -static-libgcc")
  # Prefer static libraries where possible
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  # Do not attempt dynamic link checks during try-compile in cross/static builds
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  # Avoid building any shared libraries implicitly
  set(BUILD_SHARED_LIBS OFF)
  # Build non-PIE to produce ET_EXEC static binaries; improves portability
  set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:-fno-pie> $<$<COMPILE_LANGUAGE:CXX>:-fno-pie>)
endif()

add_library(hipermap static_map.cpp cache.c static_uint64_set.c static_uint64_map.c static_domain_set.cpp static_domain_set_find.c)
option(HIPERMAP_DISABLE_SIMD "Disable SIMD codepaths in static_domain_set (force scalar fallback)" OFF)
if (HIPERMAP_DISABLE_SIMD)
  target_compile_definitions(hipermap PRIVATE HM_DISABLE_SIMD=1)
  # Suppress compiler auto-vectorization to keep fallback strictly scalar
  if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hipermap PRIVATE -fno-tree-vectorize)
  endif()
  # On x86/x86_64, also request no advanced ISA codegen
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i686|i386)$")
    target_compile_options(hipermap PRIVATE -mno-avx -mno-avx2 -mno-avx512f -mno-avx512bw -mno-sse4.2)
  endif()
  # Enforce scalar on key sources as well
  set(scalar_sources
    static_domain_set_find.c
    static_domain_set.cpp
  )
  foreach(src IN LISTS scalar_sources)
    set_source_files_properties(${src} PROPERTIES
      COMPILE_DEFINITIONS "HM_DISABLE_SIMD=1"
      COMPILE_OPTIONS "-fno-tree-vectorize;-mno-avx;-mno-avx2;-mno-avx512f;-mno-avx512bw;-mno-sse4.2")
  endforeach()
endif()
set_target_properties(hipermap PROPERTIES PUBLIC_HEADER "common.h;static_map.h;cache.h;static_uint64_set.h;static_uint64_map.h;static_domain_set.h")

# Enable ISA-specific optimizations by target architecture.
include(CheckCXXCompilerFlag)
if (MSVC)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|x64|X64)$")
    target_compile_options(hipermap PRIVATE /arch:AVX2)
    option(HIPERMAP_ENABLE_AVX512 "Enable AVX-512BW optimizations" OFF)
    if (HIPERMAP_ENABLE_AVX512)
      check_cxx_compiler_flag("/arch:AVX512" HAVE_MSVC_AVX512)
      if (HAVE_MSVC_AVX512)
        target_compile_options(hipermap PRIVATE /arch:AVX512)
      endif()
    endif()
  endif()
else()
  # Prefer feature detection over architecture strings, to behave in cross builds.
  include(CheckCXXCompilerFlag)
  if (NOT HIPERMAP_DISABLE_SIMD)
    check_cxx_compiler_flag("-mavx2" HAVE_AVX2)
  endif()
  if (HAVE_AVX2)
    target_compile_options(hipermap PRIVATE -mavx2)
  endif()
  option(HIPERMAP_ENABLE_AVX512 "Enable AVX-512BW optimizations" OFF)
  if (HIPERMAP_ENABLE_AVX512 AND NOT HIPERMAP_DISABLE_SIMD)
    check_cxx_compiler_flag("-mavx512bw" HAVE_AVX512BW)
    if (HAVE_AVX512BW)
      target_compile_options(hipermap PRIVATE -mavx512bw)
    endif()
  endif()
  # ARM64 tuning: keep baseline defaults for portability (e.g., qemu-user)
  option(HIPERMAP_ARM64_TUNE_NATIVE "Enable -mcpu=native on aarch64/arm64" OFF)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND NOT HIPERMAP_DISABLE_SIMD)
    if (HIPERMAP_ARM64_TUNE_NATIVE)
      target_compile_options(hipermap PRIVATE -mcpu=native)
    endif()
  endif()
endif()
install(
        TARGETS hipermap
        PUBLIC_HEADER DESTINATION include/hipermap
)

# Optional static_map_benchmark: requires Hyperscan, ipset and cork
option(HIPERMAP_ENABLE_STATIC_MAP_BENCHMARK "Build static_map_benchmark (fails if deps missing)" OFF)
if (HIPERMAP_ENABLE_STATIC_MAP_BENCHMARK)
  set(HAVE_HS OFF)
  set(HAVE_IPSET OFF)
  set(HAVE_CORK OFF)

  # Try pkg-config first
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    set(PKG_CONFIG_USE_STATIC_LIBS OFF)
    pkg_check_modules(HS QUIET hs libhs)
    if (HS_FOUND)
      set(HAVE_HS ON)
    endif()
    pkg_check_modules(CORK QUIET libcork cork)
    if (CORK_FOUND)
      set(HAVE_CORK ON)
    endif()
    pkg_check_modules(IPSET QUIET ipset)
    if (IPSET_FOUND)
      set(HAVE_IPSET ON)
    endif()
  endif()

  # Manual fallbacks
  if (NOT HAVE_HS)
    find_path(HYPERSCAN_INCLUDE_DIR NAMES hs/hs.h)
    find_library(HYPERSCAN_LIBRARY NAMES hs libhs)
    if (HYPERSCAN_INCLUDE_DIR AND HYPERSCAN_LIBRARY)
      set(HAVE_HS ON)
    endif()
  endif()
  if (NOT HAVE_CORK)
    # libcork headers are usually under 'libcork/...'
    find_path(CORK_INCLUDE_DIR
      NAMES libcork/core.h libcork/libcork.h cork/core.h cork.h
    )
    find_library(CORK_LIBRARY NAMES libcork cork)
    if (CORK_INCLUDE_DIR AND CORK_LIBRARY)
      set(HAVE_CORK ON)
    endif()
  endif()
  if (NOT HAVE_IPSET)
    find_path(IPSET_INCLUDE_DIR NAMES ipset/ipset.h)
    find_library(IPSET_LIBRARY NAMES ipset)
    if (IPSET_INCLUDE_DIR AND IPSET_LIBRARY)
      set(HAVE_IPSET ON)
    endif()
  endif()

  if (HAVE_HS AND HAVE_IPSET AND HAVE_CORK)
    add_executable(static_map_benchmark tools/static_map_benchmark.c)
    target_link_libraries(static_map_benchmark PRIVATE hipermap)
    install(TARGETS static_map_benchmark RUNTIME DESTINATION bin)
    # Hyperscan
    if (HS_FOUND)
      target_include_directories(static_map_benchmark PRIVATE ${HS_INCLUDE_DIRS})
      target_link_libraries(static_map_benchmark PRIVATE ${HS_LIBRARIES})
    else()
      target_include_directories(static_map_benchmark PRIVATE ${HYPERSCAN_INCLUDE_DIR})
      target_link_libraries(static_map_benchmark PRIVATE ${HYPERSCAN_LIBRARY})
    endif()
    # libcork
    if (CORK_FOUND)
      target_include_directories(static_map_benchmark PRIVATE ${CORK_INCLUDE_DIRS})
      target_link_libraries(static_map_benchmark PRIVATE ${CORK_LIBRARIES})
    else()
      target_include_directories(static_map_benchmark PRIVATE ${CORK_INCLUDE_DIR})
      target_link_libraries(static_map_benchmark PRIVATE ${CORK_LIBRARY})
    endif()
    # ipset
    if (IPSET_FOUND)
      target_include_directories(static_map_benchmark PRIVATE ${IPSET_INCLUDE_DIRS})
      target_link_libraries(static_map_benchmark PRIVATE ${IPSET_LIBRARIES})
    else()
      target_include_directories(static_map_benchmark PRIVATE ${IPSET_INCLUDE_DIR})
      target_link_libraries(static_map_benchmark PRIVATE ${IPSET_LIBRARY})
    endif()
  else()
    message(FATAL_ERROR "HIPERMAP_ENABLE_STATIC_MAP_BENCHMARK=ON but required deps not found: Hyperscan=${HAVE_HS} Cork=${HAVE_CORK} ipset=${HAVE_IPSET}")
  endif()
endif()

add_executable(test_cache tools/test_cache.c)
target_link_libraries(test_cache
  PRIVATE hipermap
)
install(TARGETS test_cache RUNTIME DESTINATION bin)

add_executable(bench_domains tools/bench_domains.cpp)
target_link_libraries(bench_domains
  PRIVATE hipermap
)
target_include_directories(bench_domains PRIVATE ${CMAKE_SOURCE_DIR})
install(TARGETS bench_domains RUNTIME DESTINATION bin)

# Optional Hyperscan integration for bench_domains
option(HIPERMAP_DOMAIN_BENCH_ENABLE_HYPERSCAN "Enable Hyperscan comparison in bench_domains" OFF)
if (HIPERMAP_DOMAIN_BENCH_ENABLE_HYPERSCAN)
  find_package(Threads)
  # Prefer pkg-config for proper transitive link flags when available
  find_package(PkgConfig)
  if (PkgConfig_FOUND)
    # Request static flags from pkg-config to improve static linking success
    set(PKG_CONFIG_USE_STATIC_LIBS ON)
    # Some distributions name the module 'hs', others 'libhs'. Try both.
    pkg_check_modules(HS QUIET hs libhs)
  endif()
  if (HS_FOUND)
    message(STATUS "Hyperscan via pkg-config: cflags='${HS_CFLAGS}' libs='${HS_LIBRARIES}'")
    target_compile_definitions(bench_domains PRIVATE BENCH_ENABLE_HYPERSCAN=1)
    target_include_directories(bench_domains PRIVATE ${HS_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/tools/domainshyperscan)
    target_sources(bench_domains PRIVATE
      ${CMAKE_SOURCE_DIR}/tools/domainshyperscan/domain_map_db.c
      ${CMAKE_SOURCE_DIR}/tools/domainshyperscan/linked_str_list.c)
    target_link_libraries(bench_domains PRIVATE ${HS_LIBRARIES} Threads::Threads)
    # Static linking disabled by default to maintain compatibility with glibc environments.
  else()
    # Fallback manual find
    find_path(HYPERSCAN_INCLUDE_DIR NAMES hs/hs.h)
    find_library(HYPERSCAN_LIBRARY NAMES hs libhs)
    if (HYPERSCAN_INCLUDE_DIR AND HYPERSCAN_LIBRARY)
      message(STATUS "Hyperscan found: include=${HYPERSCAN_INCLUDE_DIR} lib=${HYPERSCAN_LIBRARY}")
      target_compile_definitions(bench_domains PRIVATE BENCH_ENABLE_HYPERSCAN=1)
      target_include_directories(bench_domains PRIVATE
        ${HYPERSCAN_INCLUDE_DIR}
        ${HYPERSCAN_INCLUDE_DIR}/hs
        ${CMAKE_SOURCE_DIR}/tools/domainshyperscan)
      target_sources(bench_domains PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/domainshyperscan/domain_map_db.c
        ${CMAKE_SOURCE_DIR}/tools/domainshyperscan/linked_str_list.c)
      target_link_libraries(bench_domains PRIVATE ${HYPERSCAN_LIBRARY} Threads::Threads)
      # Static linking disabled by default.
    else()
      message(WARNING "Hyperscan requested but not found; building without Hyperscan support")
    endif()
  endif()
endif()

add_executable(bench_lower tools/bench_lower.cpp)
target_link_libraries(bench_lower PRIVATE hipermap)
target_include_directories(bench_lower PRIVATE ${CMAKE_SOURCE_DIR})
install(TARGETS bench_lower RUNTIME DESTINATION bin)

# Enable sanitizers to catch memory/UB issues in development builds (non-Windows only).
option(HIPERMAP_ENABLE_SANITIZERS "Enable ASan/UBSan sanitizers" OFF)
if (HIPERMAP_ENABLE_SANITIZERS AND NOT MSVC AND NOT MINGW)
  set(SAN_COMPILE_FLAGS "-fsanitize=address,undefined" "-fno-omit-frame-pointer" "-fno-sanitize-recover=undefined" "-fsanitize-address-use-after-scope")
  set(SAN_LINK_FLAGS "-fsanitize=address,undefined")
  foreach(tgt hipermap bench_domains bench_lower test_cache static_map_benchmark)
    if (TARGET ${tgt})
      target_compile_options(${tgt} PRIVATE ${SAN_COMPILE_FLAGS})
      target_link_options(${tgt} PRIVATE ${SAN_LINK_FLAGS})
    endif()
  endforeach()
endif()

# If fully static is requested, ensure all executables link fully static.
if (HIPERMAP_FULLY_STATIC AND NOT MSVC)
  foreach(tgt bench_domains bench_lower test_cache static_map_benchmark)
    if (TARGET ${tgt})
      target_link_options(${tgt} PRIVATE -static -static-libstdc++ -static-libgcc -no-pie)
    endif()
  endforeach()
endif()
