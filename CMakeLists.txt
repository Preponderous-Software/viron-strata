# viron-strata: a live, read-only 3D client for Viron, drawn with Strata
# (RFC 0030 in Preponderous-Software/rfcs).
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j4
#   ctest --test-dir build --output-on-failure
#
# Strata is fetched at the commit in strata.ref (the v0.7.0 release), or taken
# from a local checkout with -DFETCHCONTENT_SOURCE_DIR_STRATA=<path>.
# nlohmann/json is fetched as a release archive pinned by SHA-256. libcurl
# comes from the system (libcurl4-openssl-dev or libcurl4-gnutls-dev).
#
# The browser build (Emscripten + Strata's WebGL 2 backend; see web/):
#
#   emcmake cmake -S . -B build-web -DVIRON_STRATA_WEB=ON
#   cmake --build build-web -j4
#   ctest --test-dir build-web --output-on-failure   # the unit tests as wasm under Node
#
# The page is viron-strata.html + .js + .wasm, with the demo recording in
# web/demo/ built in. It fetches with emscripten_fetch instead of libcurl.
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
option(VIRON_STRATA_WEB "Build the browser page (Emscripten + Strata's WebGL 2 backend)" OFF)

if(VIRON_STRATA_WEB AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "VIRON_STRATA_WEB needs Emscripten (emcmake cmake ...)")
endif()
if(EMSCRIPTEN)
    # Strata compiles C++ exceptions as wasm exceptions; everything in one .wasm must match.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>")
    add_link_options(-fwasm-exceptions -sALLOW_MEMORY_GROWTH=1)
    # No EGL in a browser, and no desktop client (it needs libcurl).
    set(VIRON_STRATA_RENDER_TESTS OFF CACHE BOOL "" FORCE)
endif()

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

if(NOT EMSCRIPTEN)
    find_package(CURL REQUIRED)
endif()

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
add_library(viron_strata_core STATIC src/sha256.cpp src/mapping.cpp src/poller.cpp src/async_poll.cpp src/demo.cpp)
target_include_directories(viron_strata_core PUBLIC src)
target_link_libraries(viron_strata_core PUBLIC nlohmann_json::nlohmann_json)
if(NOT EMSCRIPTEN)
    # The browser build never starts the Poller's thread (AsyncPoll drives PollCore instead).
    find_package(Threads REQUIRED)
    target_link_libraries(viron_strata_core PUBLIC Threads::Threads)
endif()
viron_strata_warnings(viron_strata_core)

add_library(viron_strata_scene STATIC src/scene.cpp)
target_link_libraries(viron_strata_scene PUBLIC viron_strata_core strata::static)
viron_strata_warnings(viron_strata_scene)

if(NOT EMSCRIPTEN)
    add_library(viron_strata_curl STATIC src/transport_curl.cpp)
    target_link_libraries(viron_strata_curl PUBLIC viron_strata_core CURL::libcurl)
    viron_strata_warnings(viron_strata_curl)

    add_executable(viron-strata app/main.cpp)
    target_link_libraries(viron-strata PRIVATE viron_strata_scene viron_strata_curl)
    viron_strata_warnings(viron-strata)
endif()

if(VIRON_STRATA_WEB)
    add_executable(viron-strata-web app/web_main.cpp)
    target_link_libraries(viron-strata-web PRIVATE viron_strata_scene)
    viron_strata_warnings(viron-strata-web)
    set_target_properties(viron-strata-web PROPERTIES OUTPUT_NAME viron-strata SUFFIX ".html")
    target_link_options(viron-strata-web PRIVATE
        -sFETCH=1
        -sSTACK_SIZE=1MB
        "-sEXPORTED_FUNCTIONS=_main,_vs_demo,_vs_connect,_vs_resize,_vs_key,_vs_zoom,_vs_click,_vs_status"
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString
        "--embed-file=${CMAKE_CURRENT_SOURCE_DIR}/web/demo/recording.json@/demo/recording.json"
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html")
    set_property(TARGET viron-strata-web APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html" "${CMAKE_CURRENT_SOURCE_DIR}/web/demo/recording.json")
    # The loading shell (web/strata-kit, vendored from Strata) and its bridge go
    # inline into the page, with the download's size (web/strata_page.cmake).
    set_property(TARGET viron-strata-web APPEND PROPERTY LINK_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/web/strata-kit/strata-loader.js"
      "${CMAKE_CURRENT_SOURCE_DIR}/web/strata-boot.js")
    add_custom_command(TARGET viron-strata-web POST_BUILD
      COMMAND "${CMAKE_COMMAND}" "-DHTML=$<TARGET_FILE:viron-strata-web>"
              "-DWASM=$<TARGET_FILE_DIR:viron-strata-web>/$<TARGET_FILE_BASE_NAME:viron-strata-web>.wasm"
              "-DWEB=${CMAKE_CURRENT_SOURCE_DIR}/web" -P "${CMAKE_CURRENT_SOURCE_DIR}/web/strata_page.cmake"
      VERBATIM)
endif()

if(VIRON_STRATA_BUILD_TESTS)
    enable_testing()
    add_executable(viron_strata_tests test/test_main.cpp test/test_mapping.cpp test/test_poller.cpp test/test_world.cpp
        test/test_async.cpp test/test_demo.cpp)
    target_include_directories(viron_strata_tests PRIVATE test)
    target_link_libraries(viron_strata_tests PRIVATE viron_strata_scene)
    if(EMSCRIPTEN)
        target_link_options(viron_strata_tests PRIVATE -sNODERAWFS=1)
    else()
        target_link_libraries(viron_strata_tests PRIVATE viron_strata_curl)
    endif()
    target_compile_definitions(viron_strata_tests PRIVATE
        VIRON_STRATA_CONFORMANCE="${CMAKE_CURRENT_SOURCE_DIR}/test/conformance/viron_usd_mapping.json"
        VIRON_STRATA_DEMO_RECORDING="${CMAKE_CURRENT_SOURCE_DIR}/web/demo/recording.json")
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
