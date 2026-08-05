if(MSVC)
    set(STREAMCOMPARE_CXX_FLAGS "/O2")
else()
    set(STREAMCOMPARE_CXX_FLAGS "-O3")
endif()

set(STREAM_SCENARIO_ROOT "${CMAKE_CURRENT_LIST_DIR}/stacks")
set(STREAM_SCENARIO_ZMQ_ROOT
    "${STREAM_SCENARIO_ROOT}/zmq/libzmq_dist/linux-x64")
set(STREAM_SCENARIO_ZMQ_INCLUDE_DIR
    "${STREAM_SCENARIO_ZMQ_ROOT}/include")
set(STREAM_SCENARIO_ZMQ_LIBRARY_FILE
    "${STREAM_SCENARIO_ZMQ_ROOT}/lib/libzmq.so")

add_executable(
    bench_streamcompare_client
    ../../perf/common/streamclient/perf_stream_client.cpp)

target_link_libraries(bench_streamcompare_client PRIVATE Threads::Threads)
target_link_libraries(
    bench_streamcompare_client PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto)
target_compile_options(bench_streamcompare_client PRIVATE ${STREAMCOMPARE_CXX_FLAGS})
target_compile_features(bench_streamcompare_client PRIVATE cxx_std_17)

if(NOT TARGET test_scenario_stream_zlink)
    add_executable(
        test_scenario_stream_zlink
        ${STREAM_SCENARIO_ROOT}/zlink/test_scenario_stream_zlink.cpp)
    target_link_libraries(
        test_scenario_stream_zlink
        ${ZLINK_C_CORE_TARGET}
        Threads::Threads)
    target_include_directories(
        test_scenario_stream_zlink PRIVATE
        "${ZLINK_CORE_DIR}/include")
    set_target_properties(test_scenario_stream_zlink PROPERTIES
        BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}"
        INSTALL_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")
endif()
target_compile_options(test_scenario_stream_zlink PRIVATE ${STREAMCOMPARE_CXX_FLAGS})
target_compile_features(test_scenario_stream_zlink PRIVATE cxx_std_17)

if(NOT TARGET test_scenario_stream_zlink_packet)
    add_executable(
        test_scenario_stream_zlink_packet
        ${STREAM_SCENARIO_ROOT}/zlink_packet/test_scenario_stream_zlink_packet.cpp)
    target_link_libraries(
        test_scenario_stream_zlink_packet
        ${ZLINK_C_CORE_TARGET}
        Threads::Threads)
    target_include_directories(
        test_scenario_stream_zlink_packet PRIVATE
        "${ZLINK_CORE_DIR}/include")
    set_target_properties(test_scenario_stream_zlink_packet PROPERTIES
        BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}"
        INSTALL_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")
endif()
target_compile_options(test_scenario_stream_zlink_packet PRIVATE ${STREAMCOMPARE_CXX_FLAGS})
target_compile_features(test_scenario_stream_zlink_packet PRIVATE cxx_std_17)

if(NOT TARGET test_scenario_stream_asio)
    add_executable(
        test_scenario_stream_asio
        ${STREAM_SCENARIO_ROOT}/asio/test_scenario_stream_asio.cpp)
    target_link_libraries(test_scenario_stream_asio Threads::Threads)
endif()
target_compile_options(test_scenario_stream_asio PRIVATE ${STREAMCOMPARE_CXX_FLAGS})
target_compile_features(test_scenario_stream_asio PRIVATE cxx_std_17)

if(EXISTS "${STREAM_SCENARIO_ZMQ_INCLUDE_DIR}/zmq.h"
   AND EXISTS "${STREAM_SCENARIO_ZMQ_LIBRARY_FILE}")
    if(NOT TARGET test_scenario_stream_zmq)
        add_executable(
            test_scenario_stream_zmq
            ${STREAM_SCENARIO_ROOT}/zmq/test_scenario_stream_zmq.cpp)
        target_link_libraries(test_scenario_stream_zmq ${STREAM_SCENARIO_ZMQ_LIBRARY_FILE})
    endif()
    target_include_directories(
        test_scenario_stream_zmq PRIVATE ${STREAM_SCENARIO_ZMQ_INCLUDE_DIR})
    target_compile_options(test_scenario_stream_zmq PRIVATE ${STREAMCOMPARE_CXX_FLAGS})
    target_compile_features(test_scenario_stream_zmq PRIVATE cxx_std_17)
endif()
