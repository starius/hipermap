cmake_minimum_required(VERSION 3.25.1)

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

add_library(hipermap static_map.cpp)
set_target_properties(hipermap PROPERTIES PUBLIC_HEADER "common.h;static_map.h")
install(
        TARGETS hipermap
        PUBLIC_HEADER DESTINATION include/hipermap
)

add_executable(static_map_benchmark tools/static_map_benchmark.c)
target_link_libraries(static_map_benchmark
  PRIVATE hipermap
  PRIVATE hs
  PRIVATE ipset
  PRIVATE cork
)
