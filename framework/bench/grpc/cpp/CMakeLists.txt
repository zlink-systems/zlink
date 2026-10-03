# SPDX-License-Identifier: FSL-1.1-ALv2
#
# C++ with-grpc bench (spec: framework/bench/grpc/README.ko.md).
#
# Standalone project: it is configured and built on its own so that the build
# never overlaps a measurement window. A C++ build saturates 20 cores, and plan
# 3.2 / G7 require the measured span to contain no other build.
#
# gRPC and protobuf come from the Framework vcpkg manifest (feature "bench"), so one
# process links one protobuf.
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

get_filename_component(BENCH_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

function(read_zlink_version output file_path key)
  file(STRINGS "${file_path}" version_lines REGEX "^${key}=[0-9]+\\.[0-9]+\\.[0-9]+$")
  list(LENGTH version_lines version_line_count)
  if(NOT version_line_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one canonical ${key} version in ${file_path}")
  endif()
  string(REGEX REPLACE "^[^=]*=" "" version "${version_lines}")
  set(${output} "${version}" PARENT_SCOPE)
endfunction()

read_zlink_version(BENCH_BINDING_VERSION
  "${BENCH_REPO_ROOT}/bindings/cpp/VERSION" ZLINK_BINDING_VERSION)
read_zlink_version(BENCH_FRAMEWORK_VERSION
  "${BENCH_REPO_ROOT}/framework/languages/cpp/VERSION" ZLINK_FRAMEWORK_VERSION)
read_zlink_version(BENCH_CORE_VERSION
  "${BENCH_REPO_ROOT}/VERSION" LIBZLINK_VERSION)

find_package(Threads REQUIRED)

# --- protobuf + grpc from vcpkg ----------------------------------------------
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
message(STATUS "with-grpc bench: grpc++ ${gRPC_VERSION}, protobuf ${protobuf_VERSION}")

set(BENCH_PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../proto/bench.proto")
set(BENCH_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${BENCH_PROTO_GEN_DIR}")
set(BENCH_PROTO_CC "${BENCH_PROTO_GEN_DIR}/bench.pb.cc")
set(BENCH_PROTO_H "${BENCH_PROTO_GEN_DIR}/bench.pb.h")
set(BENCH_GRPC_CC "${BENCH_PROTO_GEN_DIR}/bench.grpc.pb.cc")
set(BENCH_GRPC_H "${BENCH_PROTO_GEN_DIR}/bench.grpc.pb.h")
add_custom_command(
  OUTPUT ${BENCH_PROTO_CC} ${BENCH_PROTO_H} ${BENCH_GRPC_CC} ${BENCH_GRPC_H}
  COMMAND protobuf::protoc
    --cpp_out=${BENCH_PROTO_GEN_DIR}
    --grpc_out=${BENCH_PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    -I${CMAKE_CURRENT_SOURCE_DIR}/../proto
    ${BENCH_PROTO_SRC}
  DEPENDS ${BENCH_PROTO_SRC} protobuf::protoc gRPC::grpc_cpp_plugin
  COMMENT "Generating with-grpc bench protobuf/gRPC sources")

# --- the C++ binding under test ---------------------------------------------
set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT "$ENV{ZLINK_LOCAL_PACKAGE_ROOT}" CACHE PATH
  "Root directory containing local package outputs")
set(ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION "${BENCH_BINDING_VERSION}" CACHE STRING
  "Exact local zlink_cpp package version consumed by this bench")
set(ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION "${BENCH_CORE_VERSION}" CACHE STRING
  "Exact local zlink Core package version consumed by zlink_cpp")
if(NOT ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT)
  set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT "${BENCH_REPO_ROOT}/.artifacts/wsl")
endif()
set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX
  "${ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT}/install/zlink-cpp/${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION}"
  CACHE PATH "Local zlink_cpp package install prefix")
set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX
  "${ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT}/install/zlink-core/${ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION}"
  CACHE PATH "Local zlink Core package install prefix")
list(PREPEND CMAKE_PREFIX_PATH
  "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}"
  "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}")
find_package(zlink_cpp ${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION} EXACT CONFIG REQUIRED)

# Consume the repository Framework through its own public CMake targets.
foreach(option_name IN ITEMS ZLINK_FRAMEWORK_CPP_BUILD_TESTS
    ZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES
    ZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE
    ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT ZLINK_STREAM_CONNECTOR_BUILD_UNREAL
    ZLINK_STREAM_CONNECTOR_BUILD_GODOT ZLINK_STREAM_CONNECTOR_BUILD_AXMOL)
  set(${option_name} OFF CACHE BOOL "Bench builds only its required libraries" FORCE)
endforeach()
add_subdirectory("${BENCH_REPO_ROOT}/framework/languages/cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/framework" EXCLUDE_FROM_ALL)
find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS sdk)

set(BENCH_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/common")

function(bench_target name)
  target_include_directories(${name} PRIVATE ${BENCH_COMMON_DIR})
  target_link_libraries(${name} PRIVATE zlink::framework)
  target_compile_definitions(${name} PRIVATE
    BENCH_FRAMEWORK_VERSION="${BENCH_FRAMEWORK_VERSION}"
    BENCH_BINDING_VERSION="${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION}"
    BENCH_CORE_VERSION="${ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION}"
    BENCH_GRPC_VERSION="${gRPC_VERSION}"
    BENCH_PROTOBUF_VERSION="${protobuf_VERSION}")
  target_compile_options(${name} PRIVATE -O3)
  set_target_properties(${name} PROPERTIES
    BUILD_RPATH "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib")
endfunction()

function(bench_grpc_target name)
  target_include_directories(${name} PRIVATE ${BENCH_PROTO_GEN_DIR})
  target_link_libraries(${name} PRIVATE gRPC::grpc++ protobuf::libprotobuf)
endfunction()

# --- servers -----------------------------------------------------------------
add_executable(bench_cpp_zlink_server zlink/bench_zlink_cpp_server.cpp ${BENCH_PROTO_CC})
target_link_libraries(bench_cpp_zlink_server PRIVATE zlink::cpp Threads::Threads)
bench_target(bench_cpp_zlink_server)
target_include_directories(bench_cpp_zlink_server PRIVATE ${BENCH_PROTO_GEN_DIR})
target_link_libraries(bench_cpp_zlink_server PRIVATE protobuf::libprotobuf)

add_executable(bench_cpp_grpc_server grpc/bench_grpc_cpp_server.cpp ${BENCH_PROTO_CC} ${BENCH_GRPC_CC})
target_link_libraries(bench_cpp_grpc_server PRIVATE Threads::Threads)
bench_target(bench_cpp_grpc_server)
bench_grpc_target(bench_cpp_grpc_server)

# --- client ------------------------------------------------------------------
add_executable(bench_cpp_client client/bench_cpp_client.cpp ${BENCH_PROTO_CC} ${BENCH_GRPC_CC})
target_link_libraries(bench_cpp_client PRIVATE zlink::cpp Threads::Threads)
bench_target(bench_cpp_client)
bench_grpc_target(bench_cpp_client)

add_executable(bench_cpp_framework_server framework/bench_framework_cpp_server.cpp ${BENCH_PROTO_CC})
target_include_directories(bench_cpp_framework_server PRIVATE ${BENCH_PROTO_GEN_DIR})
target_link_libraries(bench_cpp_framework_server PRIVATE zlink::framework_codec_protobuf)
bench_target(bench_cpp_framework_server)
target_link_libraries(bench_cpp_framework_server PRIVATE opentelemetry-cpp::metrics)
target_link_libraries(bench_cpp_client PRIVATE zlink::framework_codec_protobuf)

enable_testing()
add_executable(bench_cpp_server_metrics_test tests/server_metrics_test.cpp ${BENCH_PROTO_CC})
target_include_directories(bench_cpp_server_metrics_test PRIVATE ${BENCH_PROTO_GEN_DIR})
target_link_libraries(bench_cpp_server_metrics_test PRIVATE zlink::framework_codec_protobuf)
bench_target(bench_cpp_server_metrics_test)
target_link_libraries(bench_cpp_server_metrics_test PRIVATE opentelemetry-cpp::metrics)
add_test(NAME bench_cpp_server_rejections COMMAND bench_cpp_server_metrics_test)
add_executable(bench_cpp_raw_wire_test tests/raw_wire_test.cpp ${BENCH_PROTO_CC})
target_include_directories(bench_cpp_raw_wire_test PRIVATE ${BENCH_PROTO_GEN_DIR})
target_link_libraries(bench_cpp_raw_wire_test PRIVATE protobuf::libprotobuf)
add_test(NAME bench_cpp_raw_wire_identity COMMAND bench_cpp_raw_wire_test)
