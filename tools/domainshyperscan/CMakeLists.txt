cmake_minimum_required(VERSION 3.31)
project(domain_map C)

find_package(PkgConfig REQUIRED)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(HS REQUIRED libhs)
    if (HS_FOUND)
        message(STATUS
                "HS Libs: ${HS_LIBRARIES}"
        )
        message(STATUS
                "HS CFlags: ${HS_CFLAGS}"
        )
        add_definitions(${HS_CFLAGS})

        include_directories(${HS_INCLUDE_DIR})
        add_definitions(-DHAVE_HS)
    endif (HS_FOUND)
endif (PKG_CONFIG_FOUND)


    set(CMAKE_C_STANDARD 23)

add_executable(domain_map main.c
        domain_map_db.c
        domain_map_db.h
        linked_str_list.c
        linked_str_list.h)
target_link_libraries(domain_map PRIVATE ${HS_LIBRARIES})
target_compile_options(domain_map PRIVATE -fsanitize=address)
target_link_options(domain_map PRIVATE -fsanitize=address)
