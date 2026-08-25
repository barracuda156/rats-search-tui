cmake_minimum_required(VERSION 3.16)
project(rats-native VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(RATSN_WITH_TUI "Build the FTXUI terminal UI (native/tui/*)" ON)
option(RATSN_FILE_INDEX_DEFAULT "Default value of the fileIndex config key" ON)
# Golden-file wire-compat tests (native/tests/); OFF by default -- nothing new
# to port to the retro target, only enabled when actively verifying the M4
# peer_api codec against fixtures captured from a live Qt instance.
option(RATSN_BUILD_TESTS "Build native/tests (golden-file wire-compat checks)" OFF)
# Default ON: link an already-installed librats (e.g. the packaged MacPorts
# port) via find_package() instead of compiling src/librats from the
# vendored submodule on every build -- meaningfully cheaper on constrained
# retro hardware. Set OFF to build from the submodule, e.g. when iterating
# on librats itself alongside rats-native, or when no matching package is
# installed for the target. The vendored submodule (pinned at whatever
# commit src/librats currently points to) is what rats-native is actually
# validated against; an installed librats that has drifted from that commit
# is an untested combination -- see docs/DESIGN-native.md §6 on wire compat.
option(RATSN_USE_SYSTEM_LIBRATS "Link an installed librats via find_package() instead of building the vendored src/librats submodule (links the dynamic librats -- see rats::rats vs rats::rats_static below)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GROONGA REQUIRED IMPORTED_TARGET groonga)
# filter_policy's naming-regex filter (no Qt QRegularExpression available).
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)

if(RATSN_USE_SYSTEM_LIBRATS)
    find_package(rats CONFIG REQUIRED)
    # rats::rats (dynamic) is preferred -- matches librats' own default
    # (src/librats/CMakeLists.txt: "rats" is the shared target whenever
    # RATS_SHARED_LIBRARY is on; dynamic is macOS-idiomatic). rats::rats_static
    # is the fallback, present only when librats was built with both
    # RATS_STATIC_LIBRARY and RATS_SHARED_LIBRARY on.
    if(TARGET rats::rats)
        set(RATSN_LIBRATS_TARGET rats::rats)
    elseif(TARGET rats::rats_static)
        set(RATSN_LIBRATS_TARGET rats::rats_static)
    else()
        message(FATAL_ERROR "find_package(rats) succeeded but exported neither rats::rats nor rats::rats_static. The installed librats package config may predate the dual static/shared build in src/librats/CMakeLists.txt.")
    endif()
else()
    set(RATS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/librats)
    if(NOT EXISTS ${RATS_ROOT}/CMakeLists.txt)
        message(FATAL_ERROR "librats not found at ${RATS_ROOT}. Run: git submodule update --init --depth 1 src/librats")
    endif()
    set(RATS_BUILD_CLIENT OFF CACHE BOOL "" FORCE)
    set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RATS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # The crawler (native/engine/crawler.*) needs the BitTorrent subsystem
    # (spider mode + BEP9 metadata fetch) compiled into librats.
    set(RATS_SEARCH_FEATURES ON CACHE BOOL "" FORCE)
    add_subdirectory(${RATS_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/librats)
    set(RATSN_LIBRATS_TARGET rats)
endif()

# Bakes resources/content/{extensions,badwords}.json (the classifier's
# extension->type table and adult-content word lists, otherwise loaded from a
# Qt resource bundle) into a generated header as raw string literals, parsed
# at runtime with librats::Json. Avoids inventing an install/data-file layout
# for a target (retro PowerPC) that doesn't have one yet -- see M2 plan
# decision #3.
set(RATSN_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${RATSN_GENERATED_DIR})
set(RATSN_EXTENSIONS_JSON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../resources/content/extensions.json)
set(RATSN_BADWORDS_JSON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../resources/content/badwords.json)
# Re-run configure (and thus regenerate classifier_data.h) if either source
# JSON changes -- file(READ) alone doesn't register that dependency.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${RATSN_EXTENSIONS_JSON_PATH} ${RATSN_BADWORDS_JSON_PATH})
file(READ ${RATSN_EXTENSIONS_JSON_PATH} RATSN_EXTENSIONS_JSON)
file(READ ${RATSN_BADWORDS_JSON_PATH} RATSN_BADWORDS_JSON)
file(WRITE ${RATSN_GENERATED_DIR}/classifier_data.h
"#pragma once
// Generated at configure time from resources/content/{extensions,badwords}.json.
// Do not edit -- edit the source JSON and reconfigure instead.
namespace ratsn::domain::embedded {
inline constexpr const char* kExtensionsJson = R\"RATSN_EXT(${RATSN_EXTENSIONS_JSON})RATSN_EXT\";
inline constexpr const char* kBadwordsJson = R\"RATSN_BW(${RATSN_BADWORDS_JSON})RATSN_BW\";
} // namespace ratsn::domain::embedded
")

add_library(ratsn_core STATIC
    platform/paths.cpp
    platform/config.cpp
    platform/engine_loop.cpp
    domain/content.cpp
    domain/torrent.cpp
    domain/torrent_codec.cpp
    domain/content_classifier.cpp
    domain/filter_policy.cpp
    index/groonga_index.cpp
    engine/node_host.cpp
    engine/crawler.cpp
    engine/indexer.cpp
    engine/peer_registry.cpp
    engine/peer_api.cpp
    engine/replication.cpp
)
target_include_directories(ratsn_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${RATSN_GENERATED_DIR})
target_link_libraries(ratsn_core PUBLIC ${RATSN_LIBRATS_TARGET} PkgConfig::GROONGA PkgConfig::PCRE2)
if(RATSN_FILE_INDEX_DEFAULT)
    target_compile_definitions(ratsn_core PUBLIC RATSN_FILE_INDEX_DEFAULT=1)
else()
    target_compile_definitions(ratsn_core PUBLIC RATSN_FILE_INDEX_DEFAULT=0)
endif()
# Advertised to peers in the client_info handshake (see main.cpp kClientVersion).
target_compile_definitions(ratsn_core PUBLIC RATSN_VERSION="${PROJECT_VERSION}")

if(RATSN_WITH_TUI)
    # System-installed FTXUI only (no vendored submodule): find_package()
    # against the CMake config FTXUI's own install generates (ftxui-config.cmake,
    # exporting ftxui::screen/dom/component and the combined ftxui::ftxui).
    # Pinned to the v7.x line -- v7 renamed ScreenInteractive's underlying
    # class to App (ScreenInteractive is now just an alias) and reworked
    # parts of the component API relative to v5; code under native/tui/ is
    # written against v7.0.3 specifically; an older major won't satisfy this
    # (FTXUI's version file uses SameMajorVersion compatibility) and code
    # written against v7 isn't guaranteed to build against v5/v6 as-is.
    find_package(ftxui 7 REQUIRED CONFIG)
    target_sources(ratsn_core PRIVATE
        tui/app.cpp
        tui/search_tab.cpp
        tui/status_bar.cpp
    )
    target_link_libraries(ratsn_core PUBLIC ftxui::ftxui)
    target_compile_definitions(ratsn_core PUBLIC RATSN_WITH_TUI=1)
endif()

add_executable(ratsn main.cpp)
target_link_libraries(ratsn PRIVATE ratsn_core)

if(RATSN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
