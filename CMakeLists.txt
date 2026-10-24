# viron-strata: a live, read-only 3D client for Viron, drawn with Strata
# (RFC 0030 in Preponderous-Software/rfcs).
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j4
#   ctest --test-dir build --output-on-failure
#
# Strata is fetched at the commit in strata.ref (the v0.4.0 release), or taken
# from a local checkout with -DFETCHCONTENT_SOURCE_DIR_STRATA=<path>.
# nlohmann/json is fetched as a release archive pinned by SHA-256. libcurl
# comes from the system (libcurl4-openssl-dev or libcurl4-gnutls-dev).
cmake_minimum_required(VERSION 3.24)
project(viron_strata VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(VIRON_STRATA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VIRON_STRATA_BUILD_TESTS "Build the tests" ON)
option(VIRON_STRATA_RENDER_TESTS "Also build the headless render tests (need EGL + a GL 4.5 driver at run time)" ON)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/strata.ref" VIRON_STRATA_DEFAULT_REF)
string(STRIP "${VIRON_STRATA_DEFAULT_REF}" VIRON_STRATA_DEFAULT_REF)
set(VIRON_STRATA_STRATA_REF "${VIRON_STRATA_DEFAULT_REF}" CACHE STRING "Strata git ref (a full commit SHA; strata.ref pins it)")

include(FetchContent)
set(STRATA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(STRATA_WITH_SIM OFF CACHE BOOL "" FORCE)
set(STRATA_WITH_USD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(strata
    GIT_REPOSITORY https://github.com/Preponderous-Software/strata.git
    GIT_TAG ${VIRON_STRATA_STRATA_REF})
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(strata json)

find_package(CURL REQUIRED)

function(viron_strata_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(VIRON_STRATA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(VIRON_STRATA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# The poll loop, JSON and the mapping: no GL.
add_library(viron_strata_core STATIC src/sha256.cpp src/mapping.cpp src/poller.cpp)
target_include_directories(viron_strata_core PUBLIC src)
target_link_libraries(viron_strata_core PUBLIC nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(viron_strata_core PUBLIC Threads::Threads)
viron_strata_warnings(viron_strata_core)

add_library(viron_strata_curl STATIC src/transport_curl.cpp)
target_link_libraries(viron_strata_curl PUBLIC viron_strata_core CURL::libcurl)
viron_strata_warnings(viron_strata_curl)

add_library(viron_strata_scene STATIC src/scene.cpp)
target_link_libraries(viron_strata_scene PUBLIC viron_strata_core strata::static)
viron_strata_warnings(viron_strata_scene)

add_executable(viron-strata app/main.cpp)
target_link_libraries(viron-strata PRIVATE viron_strata_scene viron_strata_curl)
viron_strata_warnings(viron-strata)

if(VIRON_STRATA_BUILD_TESTS)
    enable_testing()
    add_executable(viron_strata_tests test/test_main.cpp test/test_mapping.cpp test/test_poller.cpp test/test_world.cpp)
    target_include_directories(viron_strata_tests PRIVATE test)
    target_link_libraries(viron_strata_tests PRIVATE viron_strata_scene viron_strata_curl)
    target_compile_definitions(viron_strata_tests PRIVATE
        VIRON_STRATA_CONFORMANCE="${CMAKE_CURRENT_SOURCE_DIR}/test/conformance/viron_usd_mapping.json")
    viron_strata_warnings(viron_strata_tests)
    add_test(NAME viron_strata_unit COMMAND viron_strata_tests "[unit]")
    add_test(NAME viron_strata_conformance COMMAND viron_strata_tests "[conformance]")
    if(VIRON_STRATA_RENDER_TESTS)
        target_sources(viron_strata_tests PRIVATE test/test_render.cpp)
        target_compile_definitions(viron_strata_tests PRIVATE
            VIRON_STRATA_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/test/golden-two-grids.txt")
        add_test(NAME viron_strata_render COMMAND viron_strata_tests "[render]")
    endif()
endif()
