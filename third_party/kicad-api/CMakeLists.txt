# Generates C++ sources from the KiCad IPC API .proto files.
# The generated code always matches the installed protobuf version, because it
# is produced at build time by that version's protoc.

set(_proto_root "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(_out_dir    "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${_out_dir}")

file(GLOB_RECURSE _protos CONFIGURE_DEPENDS RELATIVE "${_proto_root}"
     "${_proto_root}/*.proto")

set(_srcs "")
set(_hdrs "")
foreach(_p IN LISTS _protos)
    string(REGEX REPLACE "\\.proto$" "" _base "${_p}")
    list(APPEND _srcs "${_out_dir}/${_base}.pb.cc")
    list(APPEND _hdrs "${_out_dir}/${_base}.pb.h")
endforeach()

list(TRANSFORM _protos PREPEND "${_proto_root}/" OUTPUT_VARIABLE _proto_abs)

add_custom_command(
    OUTPUT ${_srcs} ${_hdrs}
    COMMAND ${BSCAN_PROTOC}
            --cpp_out=${_out_dir}
            -I${_proto_root}
            -I${BSCAN_PROTO_STD}
            ${_protos}
    WORKING_DIRECTORY "${_proto_root}"
    DEPENDS ${_proto_abs}
    COMMENT "Generating C++ from KiCad API protobuf definitions"
    VERBATIM)

add_library(kicad_api_proto STATIC ${_srcs} ${_hdrs})
target_include_directories(kicad_api_proto SYSTEM PUBLIC "${_out_dir}")
target_link_libraries(kicad_api_proto PUBLIC bscan::protobuf)
set_target_properties(kicad_api_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Generated code produces many warnings with some compiler/protobuf combinations
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(kicad_api_proto PRIVATE -w)
endif()
