cmake_minimum_required(VERSION 3.28)
project(histserv_cpp_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Ninja + Clang would otherwise try (and fail, lacking clang-scan-deps) to
# scan every TU for C++20 module dependencies we don't use.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)

find_package(ROOT REQUIRED COMPONENTS Hist)
find_package(nlohmann_json REQUIRED)

find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)

# hist.proto is not vendored here -- it's read straight out of the installed
# `histserv` Python package (a pixi/conda-forge dependency of this workspace),
# which ships its own hist.proto alongside the generated Python stubs. This
# keeps the wire protocol as a single source of truth: whatever `histserv`
# version this workspace resolves is exactly what we generate C++ stubs from,
# unmodified, with no separate checkout of the histserv repo required.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import histserv, pathlib; print(pathlib.Path(histserv.__file__).parent / 'protos' / 'hist.proto')"
    OUTPUT_VARIABLE HISTSERV_PROTO_FILE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT EXISTS "${HISTSERV_PROTO_FILE}")
    message(FATAL_ERROR "Could not locate hist.proto via the installed histserv package (looked for: ${HISTSERV_PROTO_FILE})")
endif()
get_filename_component(HISTSERV_PROTO_DIR "${HISTSERV_PROTO_FILE}" DIRECTORY)
set(PROTO_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_GENERATED_DIR}")

set(PROTO_GENERATED_SRCS
    "${PROTO_GENERATED_DIR}/hist.pb.cc"
    "${PROTO_GENERATED_DIR}/hist.grpc.pb.cc"
)
set(PROTO_GENERATED_HDRS
    "${PROTO_GENERATED_DIR}/hist.pb.h"
    "${PROTO_GENERATED_DIR}/hist.grpc.pb.h"
)

add_custom_command(
    OUTPUT ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS}
    COMMAND "${PROTOC_EXECUTABLE}"
        "--cpp_out=${PROTO_GENERATED_DIR}"
        "--grpc_out=${PROTO_GENERATED_DIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}"
        -I "${HISTSERV_PROTO_DIR}"
        "${HISTSERV_PROTO_FILE}"
    DEPENDS "${HISTSERV_PROTO_FILE}"
    COMMENT "Generating C++ protobuf/gRPC code from histserv's hist.proto"
    VERBATIM
)

add_library(histserv_proto STATIC ${PROTO_GENERATED_SRCS})
target_include_directories(histserv_proto PUBLIC "${PROTO_GENERATED_DIR}")
target_link_libraries(histserv_proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)

add_library(histserv_client
    src/root_hist_serialize.cpp
    src/hist_serv_client.cpp
)
target_include_directories(histserv_client PUBLIC src)
target_link_libraries(histserv_client PUBLIC
    histserv_proto
    ROOT::Hist
    nlohmann_json::nlohmann_json
)

add_executable(histserv_demo src/main.cpp)
target_link_libraries(histserv_demo PRIVATE histserv_client)
