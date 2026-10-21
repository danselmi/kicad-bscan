# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.20)

project(kicad-bscan
    VERSION 0.1.0
    DESCRIPTION "Boundary-scan viewer plugin for the KiCad PCB editor"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release by default: debug information is of no use to plugin users, and
# with GCC/MinGW it is kept in the .exe (48 MB instead of 2 MB on Windows).
# For debugging: -DCMAKE_BUILD_TYPE=RelWithDebInfo (or Debug).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BSCAN_BUILD_GUI   "Build the wxWidgets plugin window"            ON)
option(BSCAN_BUILD_TOOLS "Build the developer tools (bscan-chain, bscan-kicad-probe)" OFF)
option(BSCAN_BUILD_TESTS "Build the fake KiCad server used for testing" OFF)

# Identifier shown to KiCad. KiCad skips plugins with an identifier it does
# not accept, silently (visible only in the KICAD_API trace: "identifier ...
# does not meet requirements"). The rules differ between versions:
#   KiCad 10: ^[a-zA-Z]{2,}(\.([a-zA-Z0-9][a-zA-Z0-9-]*[a-zA-Z0-9]|[a-zA-Z0-9])){2,}$
#             (no '_'; '-' only inside a part)
#   KiCad 9:  [\w\d]{2,}\.[\w\d]+\.[\w\d]+ somewhere in the identifier
# Both are checked below.
set(_bscan_default_id "com.github.danselmi.kicad-bscan")
set(BSCAN_PLUGIN_ID "${_bscan_default_id}" CACHE STRING "KiCad plugin identifier")
# Earlier defaults, rejected by KiCad 9 and 10 or by KiCad 10
if(BSCAN_PLUGIN_ID STREQUAL "bscan.kicad-bscan" OR BSCAN_PLUGIN_ID STREQUAL "com.github.danselmi.kicad_bscan")
    set(BSCAN_PLUGIN_ID "${_bscan_default_id}" CACHE STRING "KiCad plugin identifier" FORCE)
endif()
set(_id_part "[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?")
if(NOT BSCAN_PLUGIN_ID MATCHES "^[A-Za-z][A-Za-z]+\\.${_id_part}(\\.${_id_part})+$"
   OR NOT BSCAN_PLUGIN_ID MATCHES "[A-Za-z0-9_][A-Za-z0-9_]+\\.[A-Za-z0-9_]+\\.[A-Za-z0-9_]+")
    message(FATAL_ERROR "BSCAN_PLUGIN_ID '${BSCAN_PLUGIN_ID}' is not accepted by KiCad 9 and 10: "
                        "use a reverse-DNS identifier with at least three parts, a first part "
                        "of letters only, parts of letters, digits and '-' (not at the start "
                        "or end of a part), no '_', e.g. com.github.user.my-plugin")
endif()
set(BSCAN_PLUGIN_NAME "Boundary Scan"     CACHE STRING "KiCad plugin display name")

# Where 'cmake --install' puts the plugin: the per-user plugin folder of the
# installed KiCad version (<documents>/<major.minor>/plugins).
#
# Version: from 'kicad-cli version', else KICAD_VERSION (default 10.0).
# Folder: Linux uses ~/.local/share/kicad (KiCad's own documentation also
# mentions ~/.local/share/KiCad; an existing one of the two is preferred).
set(KICAD_VERSION "" CACHE STRING "KiCad version (major.minor) to install for; empty: detect")

if(NOT KICAD_VERSION)
    find_program(KICAD_CLI kicad-cli)
    if(KICAD_CLI)
        execute_process(COMMAND "${KICAD_CLI}" version
                        OUTPUT_VARIABLE _kicad_ver OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _kicad_rv)
        if(_kicad_rv EQUAL 0 AND _kicad_ver MATCHES "([0-9]+)\\.([0-9]+)")
            set(_kicad_series "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
        endif()
    endif()
    if(NOT _kicad_series)
        set(_kicad_series "10.0")
    endif()
else()
    set(_kicad_series "${KICAD_VERSION}")
endif()

if(WIN32)
    set(_kicad_docs "$ENV{USERPROFILE}/Documents/KiCad")
elseif(APPLE)
    set(_kicad_docs "$ENV{HOME}/Documents/KiCad")
elseif(EXISTS "$ENV{HOME}/.local/share/KiCad" AND NOT EXISTS "$ENV{HOME}/.local/share/kicad")
    set(_kicad_docs "$ENV{HOME}/.local/share/KiCad")
else()
    set(_kicad_docs "$ENV{HOME}/.local/share/kicad")
endif()

set(KICAD_PLUGIN_DIR "${_kicad_docs}/${_kicad_series}/plugins" CACHE PATH
    "KiCad IPC plugin directory (the plugin goes into a subdirectory)")
message(STATUS "KiCad plugin folder: ${KICAD_PLUGIN_DIR}")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(cmake/FindDeps.cmake)

# ---------------------------------------------------------------------------
# KiCad API: generate C++ from the .proto files
# ---------------------------------------------------------------------------
add_subdirectory(third_party/kicad-api)

# ---------------------------------------------------------------------------
# BSDL parser (git submodule third_party/libbsdl, static library)
# ---------------------------------------------------------------------------
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libbsdl/CMakeLists.txt)
    message(FATAL_ERROR "libbsdl is missing: run 'git submodule update --init'")
endif()
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/libbsdl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# OpenOCD client + boundary-scan logic (no GUI dependency)
# ---------------------------------------------------------------------------
add_library(bscan_core STATIC
    src/openocd/tcl_client.cpp
    src/bscan/bits.cpp
    src/bscan/bsdl_device.cpp
    src/bscan/chain.cpp
    src/bscan/project_config.cpp
    src/bscan/tap_status.cpp
    src/bscan/extest_job.cpp
    src/bscan/extest_table.cpp
    src/bscan/live_sampler.cpp
    src/bscan/sample_job.cpp
    src/bscan/extest.cpp
    src/bscan/bsdl_index.cpp
    src/bscan/interconnect.cpp
    src/bscan/interconnect_job.cpp
    src/bscan/interconnect_session.cpp)
target_include_directories(bscan_core PUBLIC src)
target_link_libraries(bscan_core PUBLIC bsdl PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(bscan_core PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# KiCad IPC client (no GUI dependency)
# ---------------------------------------------------------------------------
add_library(bscan_kicad STATIC
    src/kicad/ipc_client.cpp
    src/kicad/board_watcher.cpp)
target_include_directories(bscan_kicad PUBLIC src)
target_link_libraries(bscan_kicad PUBLIC kicad_api_proto bscan::nng)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bscan_kicad PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(bscan_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
# Developer tools: not installed, see doc/DEVELOPMENT.md
if(BSCAN_BUILD_TOOLS)
    add_executable(bscan-kicad-probe tools/kicad_probe.cpp)
    target_link_libraries(bscan-kicad-probe PRIVATE bscan_kicad)

    add_executable(bscan-chain tools/bscan_chain.cpp)
    target_link_libraries(bscan-chain PRIVATE bscan_core)
endif()

if(BSCAN_BUILD_TESTS)
    add_executable(fake-kicad tests/fake_kicad.cpp)
    target_link_libraries(fake-kicad PRIVATE kicad_api_proto bscan::nng)

    enable_testing()
    add_executable(test-bscan tests/test_bscan.cpp)
    target_link_libraries(test-bscan PRIVATE bscan_core)
    target_compile_definitions(test-bscan PRIVATE
        SIMCHIP_BSD="${PROJECT_SOURCE_DIR}/tests/sim/simchip.bsd"
        SHARED_BSD="${PROJECT_SOURCE_DIR}/tests/data/shared_ctrl.bsd"
        SIMCHIP2_BSD="${PROJECT_SOURCE_DIR}/tests/sim/simchip2.bsd")
    add_test(NAME bscan COMMAND test-bscan)

    # Needs a running OpenOCD (e.g. with tests/sim), therefore not a ctest
    add_executable(extest-demo tests/extest_demo.cpp)
    target_link_libraries(extest-demo PRIVATE bscan_core)

    # Two devices in EXTEST; needs OpenOCD with 'jtag scan' and tests/sim
    add_executable(multi-extest-demo tests/multi_extest_demo.cpp)
    target_link_libraries(multi-extest-demo PRIVATE bscan_core)

    # Interconnect test through the sampler; needs OpenOCD and tests/sim
    add_executable(interconnect-demo tests/interconnect_demo.cpp)
    target_link_libraries(interconnect-demo PRIVATE bscan_core)
endif()

if(BSCAN_BUILD_GUI)
    add_subdirectory(src/gui)
endif()
