cmake_minimum_required(VERSION 3.10)
project(zlink_cpp_perf LANGUAGES CXX)

if(NOT DEFINED ZLINK_CPP_CORE_TARGET)
  message(FATAL_ERROR "ZLINK_CPP_CORE_TARGET is not configured")
endif()

find_package(Threads REQUIRED)

set(PERF_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PERF_CPP_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")
set(PERF_CORE_INCLUDE_DIR "${ZLINK_CORE_DIR}/include")

set(PERF_SINGLE_COMMON_SRCS
  "${PERF_ROOT_DIR}/single/common/perf_single_common.cpp"
  "${PERF_ROOT_DIR}/single/common/perf_single_runner.cpp"
)

function(configure_perf_target target_name out_subdir)
  target_compile_features(${target_name} PRIVATE cxx_std_20)
  target_link_libraries(${target_name} PRIVATE
    zlink_cpp
    Threads::Threads)
  target_include_directories(${target_name} PRIVATE
    "${PERF_CORE_INCLUDE_DIR}"
    "${PERF_CPP_INCLUDE_DIR}"
    "${PERF_ROOT_DIR}/single/common"
    "${PERF_ROOT_DIR}/multi/common"
  )
  if(UNIX AND NOT APPLE)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${ZLINK_CPP_RUNTIME_DIR}"
    )
  elseif(APPLE)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${ZLINK_CPP_RUNTIME_DIR}"
    )
  endif()

  if(MSVC)
    target_compile_options(${target_name} PRIVATE /O2 /W4)
  else()
    target_compile_options(${target_name} PRIVATE -O3 -Wall -Wextra -Wpedantic)
  endif()

  set_target_properties(${target_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PERF_ROOT_DIR}/${out_subdir}/build"
  )
endfunction()

function(add_cpp_single_bench target_name src_path)
  add_executable(${target_name}
    ${PERF_SINGLE_COMMON_SRCS}
    "${src_path}"
  )
  configure_perf_target(${target_name} "single")
endfunction()

function(add_cpp_multi_server_bench target_name src_path)
  add_executable(${target_name}
    "${src_path}"
  )
  configure_perf_target(${target_name} "multi")
endfunction()

function(add_cpp_multi_client_bench target_name src_path)
  add_executable(${target_name}
    "${src_path}"
  )
  configure_perf_target(${target_name} "multi")
endfunction()

# Single suite
add_cpp_single_bench(cpp_perf_pair
  "${PERF_ROOT_DIR}/single/src/perf_pair.cpp")
add_cpp_single_bench(cpp_perf_pubsub
  "${PERF_ROOT_DIR}/single/src/perf_pubsub.cpp")
add_cpp_single_bench(cpp_perf_dealer_dealer
  "${PERF_ROOT_DIR}/single/src/perf_dealer_dealer.cpp")
add_cpp_single_bench(cpp_perf_dealer_router
  "${PERF_ROOT_DIR}/single/src/perf_dealer_router.cpp")
add_cpp_single_bench(cpp_perf_dealer_router_reqrep
  "${PERF_ROOT_DIR}/single/src/perf_dealer_router_reqrep.cpp")
add_cpp_single_bench(cpp_perf_router_router
  "${PERF_ROOT_DIR}/single/src/perf_router_router.cpp")
add_cpp_single_bench(cpp_perf_router_router_reqrep
  "${PERF_ROOT_DIR}/single/src/perf_router_router_reqrep.cpp")
# Multi server suite
add_cpp_multi_server_bench(cpp_comp_src_dealer_dealer_server
  "${PERF_ROOT_DIR}/multi/src/perf_dealer_dealer_server.cpp")
add_cpp_multi_server_bench(cpp_comp_src_dealer_router_server
  "${PERF_ROOT_DIR}/multi/src/perf_dealer_router_server.cpp")
add_cpp_multi_server_bench(cpp_comp_src_dealer_router_reqrep_server
  "${PERF_ROOT_DIR}/multi/src/perf_dealer_router_reqrep_server.cpp")
add_cpp_multi_server_bench(cpp_comp_src_router_router_server
  "${PERF_ROOT_DIR}/multi/src/perf_router_router_server.cpp")
add_cpp_multi_server_bench(cpp_comp_src_router_router_reqrep_server
  "${PERF_ROOT_DIR}/multi/src/perf_router_router_reqrep_server.cpp")
add_cpp_multi_server_bench(cpp_comp_src_pubsub_server
  "${PERF_ROOT_DIR}/multi/src/perf_pubsub_server.cpp")
add_cpp_multi_server_bench(cpp_comp_src_stream_server
  "${PERF_ROOT_DIR}/multi/src/perf_stream_server.cpp")
configure_perf_target(cpp_comp_src_stream_server "multi")

# Multi client suite
add_cpp_multi_client_bench(cpp_comp_src_dealer_dealer_client
  "${PERF_ROOT_DIR}/multi/src/perf_dealer_dealer_client.cpp")
add_cpp_multi_client_bench(cpp_comp_src_dealer_router_client
  "${PERF_ROOT_DIR}/multi/src/perf_dealer_router_client.cpp")
add_cpp_multi_client_bench(cpp_comp_src_dealer_router_reqrep_client
  "${PERF_ROOT_DIR}/multi/src/perf_dealer_router_reqrep_client.cpp")
add_cpp_multi_client_bench(cpp_comp_src_router_router_client
  "${PERF_ROOT_DIR}/multi/src/perf_router_router_client.cpp")
add_cpp_multi_client_bench(cpp_comp_src_router_router_reqrep_client
  "${PERF_ROOT_DIR}/multi/src/perf_router_router_reqrep_client.cpp")
add_cpp_multi_client_bench(cpp_comp_src_pubsub_client
  "${PERF_ROOT_DIR}/multi/src/perf_pubsub_client.cpp")
if(ZLINK_CPP_REGISTER_CTEST AND UNIX AND NOT WIN32)
  add_test(NAME perf_cpp_multi_dealer_dealer_runtime_smoke
    COMMAND /bin/bash -lc
      "PERF_FAIL_FAST=1 PERF_TIMEOUT_SECONDS=20 ${PERF_ROOT_DIR}/run_binding_multi.sh --reuse-build --pattern DEALER_DEALER --transports tcp --msg-sizes 64 --duration 1 --runs 1 --clients 2 --connect-concurrency 2 --results-tag ctest_cpp_multi_dealer_dealer_runtime_smoke"
  )
  set_tests_properties(perf_cpp_multi_dealer_dealer_runtime_smoke PROPERTIES
    LABELS "perf-regression"
    TIMEOUT 90
    WORKING_DIRECTORY "${ZLINK_REPO_ROOT_DIR}"
  )

endif()
