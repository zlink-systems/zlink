# bindings/c/perf - current zlink C binding benchmark targets

if(NOT TARGET zlink_c)
    message(FATAL_ERROR "C binding target not found. Build bindings/c first.")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Python3 COMPONENTS Interpreter)

get_filename_component(ZLINK_C_BINDINGS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(ZLINK_REPO_ROOT_DIR "${ZLINK_C_BINDINGS_DIR}/../.." ABSOLUTE)

set(PERF_BINDINGS_C_INCLUDE_DIR "${ZLINK_C_BINDINGS_DIR}/include")

set(PERF_EXTERNAL_INCLUDE_DIR "${ZLINK_REPO_ROOT_DIR}/core/external")
if(NOT EXISTS "${PERF_EXTERNAL_INCLUDE_DIR}")
    set(PERF_EXTERNAL_INCLUDE_DIR "${ZLINK_REPO_ROOT_DIR}/external")
endif()
set(PERF_BENCH_SINGLE_COMMON_DIR
    "${ZLINK_C_BINDINGS_DIR}/bench/with_zmq/single/common")
set(PERF_BENCH_MULTI_COMMON_DIR
    "${ZLINK_C_BINDINGS_DIR}/bench/with_zmq/multi/common")

if(MSVC)
    set(PERF_CXX_FLAGS "/O2")
else()
    set(PERF_CXX_FLAGS "-O3")
endif()

function(configure_current_bench_runtime target_name)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE Psapi)
    endif()
endfunction()

function(add_current_bench_single name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE zlink_c Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(${name} PRIVATE dl)
    endif()
    target_include_directories(${name} PRIVATE
        ${PERF_BINDINGS_C_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/single/common
        ${PERF_EXTERNAL_INCLUDE_DIR})
    target_compile_options(${name} PRIVATE ${PERF_CXX_FLAGS})
    target_compile_features(${name} PRIVATE cxx_std_11)
    configure_current_bench_runtime(${name})
endfunction()

function(add_current_bench_single_recv name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE zlink_c Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(${name} PRIVATE dl)
    endif()
    target_include_directories(${name} PRIVATE
        ${PERF_BINDINGS_C_INCLUDE_DIR}
        ${PERF_BENCH_SINGLE_COMMON_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/single/common
        ${PERF_EXTERNAL_INCLUDE_DIR})
    target_compile_options(${name} PRIVATE ${PERF_CXX_FLAGS})
    target_compile_features(${name} PRIVATE cxx_std_11)
    configure_current_bench_runtime(${name})
endfunction()

function(add_current_bench_multi name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE zlink_c Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(${name} PRIVATE dl)
    endif()
    target_include_directories(${name} PRIVATE
        ${PERF_BINDINGS_C_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/multi/common
        ${PERF_BENCH_MULTI_COMMON_DIR})
    target_compile_options(${name} PRIVATE ${PERF_CXX_FLAGS})
    target_compile_features(${name} PRIVATE cxx_std_11)
    configure_current_bench_runtime(${name})
endfunction()

    add_current_bench_single(perf_pair single/src/perf_pair.cpp)
    add_current_bench_single(perf_pubsub single/src/perf_pubsub.cpp)
    add_current_bench_single(perf_dealer_dealer single/src/perf_dealer_dealer.cpp)
    add_current_bench_single(perf_dealer_router single/src/perf_dealer_router.cpp)
    add_current_bench_single(perf_dealer_router_reqrep single/src/perf_dealer_router_reqrep.cpp)
    add_current_bench_single(perf_router_router single/src/perf_router_router.cpp)
    add_current_bench_single(perf_router_router_reqrep single/src/perf_router_router_reqrep.cpp)

    add_executable(
        perf_stream_client
        common/streamclient/perf_stream_client.cpp)
    target_link_libraries(
        perf_stream_client PRIVATE
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto)
    target_include_directories(
        perf_stream_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common/streamclient
        ${PERF_EXTERNAL_INCLUDE_DIR}/boost)
    target_compile_options(perf_stream_client PRIVATE ${PERF_CXX_FLAGS})
    target_compile_features(perf_stream_client PRIVATE cxx_std_17)

    add_current_bench_multi(comp_src_dealer_dealer_server
                           multi/src/perf_multi_dealer_dealer_server.cpp)
    add_current_bench_multi(comp_src_dealer_router_sendsend_server
                           multi/src/perf_multi_dealer_router_server.cpp)
    add_current_bench_multi(comp_src_dealer_router_reqrep_server
                           multi/src/perf_multi_dealer_router_reqrep_server.cpp)
    add_current_bench_multi(comp_src_router_router_sendsend_server
                           multi/src/perf_multi_router_router_server.cpp)
    add_current_bench_multi(comp_src_router_router_reqrep_server
                           multi/src/perf_multi_router_router_reqrep_server.cpp)
    add_current_bench_multi(comp_src_router_router_oneway_server
                           multi/src/perf_multi_router_router_oneway_server.cpp)
    add_current_bench_multi(comp_src_pubsub_server
                           multi/src/perf_multi_pubsub_server.cpp)
    add_current_bench_multi(comp_src_dealer_dealer_client
                           multi/src/perf_multi_dealer_dealer_client.cpp)
    add_current_bench_multi(comp_src_dealer_router_sendsend_client
                           multi/src/perf_multi_dealer_router_client.cpp)
    add_current_bench_multi(comp_src_dealer_router_reqrep_client
                           multi/src/perf_multi_dealer_router_reqrep_client.cpp)
    add_current_bench_multi(comp_src_router_router_sendsend_client
                           multi/src/perf_multi_router_router_client.cpp)
    add_current_bench_multi(comp_src_router_router_reqrep_client
                           multi/src/perf_multi_router_router_reqrep_client.cpp)
    foreach(matched_pattern IN ITEMS router_router_reqrep router_router_sendsend)
        string(TOUPPER "${matched_pattern}" matched_pattern_upper)
        add_current_bench_multi(
            comp_src_${matched_pattern}_matched_client
            multi/src/perf_multi_router_router_matched_client.cpp)
        target_compile_definitions(
            comp_src_${matched_pattern}_matched_client
            PRIVATE PERF_MATCHED_PATTERN_NAME="MULTI_${matched_pattern_upper}")
    endforeach()
    add_current_bench_multi(comp_src_router_router_oneway_client
                           multi/src/perf_multi_router_router_oneway_client.cpp)
    add_current_bench_multi(comp_src_pubsub_client
                           multi/src/perf_multi_pubsub_client.cpp)
    add_current_bench_multi(perf_multi_metrics_test
                           multi/tests/test_perf_multi_metrics.cpp)
    if(ZLINK_C_REGISTER_CTEST)
        add_test(NAME perf_multi_metrics_test COMMAND perf_multi_metrics_test)
        set_tests_properties(perf_multi_metrics_test PROPERTIES
            LABELS "perf-metric"
            TIMEOUT 30)
    endif()
    add_current_bench_multi(comp_src_stream_server
                           multi/src/perf_multi_stream_server.cpp)
target_compile_definitions(comp_src_stream_server PRIVATE
                           PERF_MULTI_STREAM_PATTERN_NAME="MULTI_STREAM")
