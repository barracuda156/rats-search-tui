cmake_minimum_required(VERSION 3.16)
project(rats-native VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# M3 adds native/tui/*; nothing to build against it yet.
option(RATSN_WITH_TUI "Build the FTXUI terminal UI" OFF)
option(RATSN_FILE_INDEX_DEFAULT "Default value of the fileIndex config key" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GROONGA REQUIRED IMPORTED_TARGET groonga)

set(RATS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/librats)
if(NOT EXISTS ${RATS_ROOT}/CMakeLists.txt)
    message(FATAL_ERROR "librats not found at ${RATS_ROOT}. Run: git submodule update --init --depth 1 src/librats")
endif()
set(RATS_BUILD_CLIENT OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${RATS_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/librats)

add_library(ratsn_core STATIC
    platform/paths.cpp
    platform/config.cpp
    platform/engine_loop.cpp
    domain/content.cpp
    domain/torrent.cpp
    domain/torrent_codec.cpp
    index/groonga_index.cpp
)
target_include_directories(ratsn_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ratsn_core PUBLIC rats PkgConfig::GROONGA)
if(RATSN_FILE_INDEX_DEFAULT)
    target_compile_definitions(ratsn_core PUBLIC RATSN_FILE_INDEX_DEFAULT=1)
else()
    target_compile_definitions(ratsn_core PUBLIC RATSN_FILE_INDEX_DEFAULT=0)
endif()

add_executable(ratsn main.cpp)
target_link_libraries(ratsn PRIVATE ratsn_core)

if(RATSN_WITH_TUI)
    message(FATAL_ERROR "RATSN_WITH_TUI is not implemented until M3")
endif()
