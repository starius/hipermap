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

add_executable(main main.c)

target_link_libraries(main
  PRIVATE hipermap
  PRIVATE hs
  PRIVATE ipset
  PRIVATE cork
)