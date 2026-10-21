# SPDX-License-Identifier: GPL-3.0-only
# Locates Protobuf and NNG and normalises them to these names:
#
#   bscan::protobuf   - the protobuf runtime library
#   BSCAN_PROTOC      - path or target of the protoc compiler
#   BSCAN_PROTO_STD   - include dir containing google/protobuf/*.proto
#   bscan::nng        - the NNG library
#
# and requires nlohmann_json (target nlohmann_json::nlohmann_json).
#
# Protobuf: newer releases (4.x / 5.x / 6.x, which need Abseil) ship a CMake
# package config; older ones (e.g. 3.19 in Fedora <= 44) are found through
# CMake's FindProtobuf module. Both paths are supported.

# --- Protobuf ---------------------------------------------------------------
find_package(Protobuf CONFIG QUIET)

if(Protobuf_FOUND AND TARGET protobuf::libprotobuf)
    message(STATUS "Protobuf ${Protobuf_VERSION} (CMake config package)")
else()
    find_package(Protobuf MODULE REQUIRED)
    message(STATUS "Protobuf ${Protobuf_VERSION} (FindProtobuf module)")
endif()

add_library(bscan::protobuf INTERFACE IMPORTED)
target_link_libraries(bscan::protobuf INTERFACE protobuf::libprotobuf)

if(TARGET protobuf::protoc)
    set(BSCAN_PROTOC protobuf::protoc)
elseif(Protobuf_PROTOC_EXECUTABLE)
    set(BSCAN_PROTOC "${Protobuf_PROTOC_EXECUTABLE}")
else()
    find_program(BSCAN_PROTOC protoc REQUIRED)
endif()

# Directory with google/protobuf/any.proto etc. (well-known types)
set(BSCAN_PROTO_STD "")
get_target_property(_pb_inc protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
foreach(_d IN LISTS _pb_inc Protobuf_INCLUDE_DIRS Protobuf_INCLUDE_DIR)
    if(_d AND EXISTS "${_d}/google/protobuf/any.proto")
        set(BSCAN_PROTO_STD "${_d}")
        break()
    endif()
endforeach()
if(NOT BSCAN_PROTO_STD)
    message(FATAL_ERROR "google/protobuf/any.proto not found; is the protobuf "
                        "development package complete?")
endif()

# --- nlohmann/json (header only; Fedora: json-devel) ------------------------
find_package(nlohmann_json 3.2 REQUIRED)

# --- NNG --------------------------------------------------------------------
find_package(nng CONFIG QUIET)

if(TARGET nng::nng)
    message(STATUS "NNG ${nng_VERSION} (CMake config package)")
    add_library(bscan::nng INTERFACE IMPORTED)
    target_link_libraries(bscan::nng INTERFACE nng::nng)
else()
    find_path(NNG_INCLUDE_DIR nng/nng.h REQUIRED)
    find_library(NNG_LIBRARY nng REQUIRED)
    message(STATUS "NNG: ${NNG_LIBRARY}")
    add_library(bscan::nng UNKNOWN IMPORTED)
    set_target_properties(bscan::nng PROPERTIES
        IMPORTED_LOCATION "${NNG_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NNG_INCLUDE_DIR}")
endif()
