# SPDX-License-Identifier: FSL-1.1-ALv2
#
# C++ with-grpc bench (spec: framework/doc/framework/common/bench/with-grpc-local.ko.md).
#
# Standalone project: it is configured and built on its own so that the build
# never overlaps a measurement window. A C++ build saturates 20 cores, and plan
# 3.2 / G7 require the measured span to contain no other build.
#
# spec 8.1: gRPC comes from the SYSTEM libgrpc++ and grpc_cpp_plugin (1.51.1 on
# this machine). vcpkg does not build gRPC here.
cmake_minimum_required(VERSION 3.20)
project(zlink_cpp_with_grpc_bench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  # spec 3: Release build. Measuring a Debug binding would answer a different
  # question than the one the campaign asks.
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

get_filename_component(BENCH_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# --- protobuf + grpc from the system ----------------------------------------
find_program(BENCH_PROTOC protoc REQUIRED)
find_program(BENCH_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
pkg_check_modules(BENCH_GRPCPP REQUIRED grpc++)
pkg_check_modules(BENCH_PROTOBUF REQUIRED protobuf)
message(STATUS "with-grpc bench: grpc++ ${BENCH_GRPCPP_VERSION}, protobuf ${BENCH_PROTOBUF_VERSION}")

set(BENCH_PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/bench.proto")
set(BENCH_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${BENCH_PROTO_GEN_DIR}")
set(BENCH_PROTO_CC "${BENCH_PROTO_GEN_DIR}/bench.pb.cc")
set(BENCH_PROTO_H "${BENCH_PROTO_GEN_DIR}/bench.pb.h")
set(BENCH_GRPC_CC "${BENCH_PROTO_GEN_DIR}/bench.grpc.pb.cc")
set(BENCH_GRPC_H "${BENCH_PROTO_GEN_DIR}/bench.grpc.pb.h")
add_custom_command(
  OUTPUT ${BENCH_PROTO_CC} ${BENCH_PROTO_H} ${BENCH_GRPC_CC} ${BENCH_GRPC_H}
  COMMAND ${BENCH_PROTOC}
    --cpp_out=${BENCH_PROTO_GEN_DIR}
    --grpc_out=${BENCH_PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${BENCH_GRPC_CPP_PLUGIN}
    -I${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${BENCH_PROTO_SRC}
  DEPENDS ${BENCH_PROTO_SRC}
  COMMENT "Generating with-grpc bench protobuf/gRPC sources")

# --- the C++ binding under test ---------------------------------------------
set(ZLINK_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZLINK_CPP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ZLINK_CORE_DIR "${BENCH_REPO_ROOT}/core" CACHE PATH "" FORCE)
if(NOT DEFINED ZLINK_CPP_CORE_BUILD_DIR)
  set(ZLINK_CPP_CORE_BUILD_DIR "${BENCH_REPO_ROOT}/core/build-dev" CACHE PATH "" FORCE)
endif()
add_subdirectory("${BENCH_REPO_ROOT}/bindings/cpp" "${CMAKE_CURRENT_BINARY_DIR}/zlink_cpp")

set(BENCH_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")

function(bench_target name)
  target_include_directories(${name} PRIVATE ${BENCH_COMMON_DIR})
  target_compile_options(${name} PRIVATE -O3)
  set_target_properties(${name} PROPERTIES
    BUILD_RPATH "${ZLINK_CPP_CORE_BUILD_DIR}/lib;${ZLINK_CPP_CORE_BUILD_DIR}/bin")
endfunction()

function(bench_grpc_target name)
  target_include_directories(${name} PRIVATE
    ${BENCH_PROTO_GEN_DIR} ${BENCH_GRPCPP_INCLUDE_DIRS} ${BENCH_PROTOBUF_INCLUDE_DIRS})
  target_link_directories(${name} PRIVATE
    ${BENCH_GRPCPP_LIBRARY_DIRS} ${BENCH_PROTOBUF_LIBRARY_DIRS})
  target_link_libraries(${name} PRIVATE
    ${BENCH_GRPCPP_LIBRARIES} ${BENCH_PROTOBUF_LIBRARIES})
endfunction()

# --- servers -----------------------------------------------------------------
add_executable(bench_cpp_zlink_server zlink/bench_zlink_cpp_server.cpp)
target_link_libraries(bench_cpp_zlink_server PRIVATE zlink_cpp Threads::Threads)
bench_target(bench_cpp_zlink_server)

add_executable(bench_cpp_grpc_server grpc/bench_grpc_cpp_server.cpp ${BENCH_PROTO_CC} ${BENCH_GRPC_CC})
target_link_libraries(bench_cpp_grpc_server PRIVATE Threads::Threads)
bench_target(bench_cpp_grpc_server)
bench_grpc_target(bench_cpp_grpc_server)

# --- client ------------------------------------------------------------------
add_executable(bench_cpp_client client/bench_cpp_client.cpp ${BENCH_PROTO_CC} ${BENCH_GRPC_CC})
target_link_libraries(bench_cpp_client PRIVATE zlink_cpp Threads::Threads)
bench_target(bench_cpp_client)
bench_grpc_target(bench_cpp_client)
