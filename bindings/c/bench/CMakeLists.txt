find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

option(ZLINK_C_BUILD_NATIVE_BENCHES_ONLY
       "Build comparison benches supported by the C binding and bundled headers"
       OFF)

set(ZLINK_C_BENCH_BOOST_INCLUDE_DIR "${ZLINK_REPO_ROOT_DIR}/core/external/boost")

option(ZLINK_C_BUILD_BENCH_ZMQ
       "Build bindings/c/bench/with_zmq targets"
       ${ZLINK_C_BUILD_BENCHES})
option(ZLINK_C_BUILD_BENCH_STREAMCOMPARE
       "Build bindings/c/bench/with_stream targets"
       ${ZLINK_C_BUILD_BENCHES})
option(ZLINK_C_BUILD_BENCH_ROUTER_COMPARE
       "Build bindings/c/bench/with_routing targets"
       ${ZLINK_C_BUILD_BENCHES})

if(ZLINK_C_BUILD_BENCH_ZMQ
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/with_zmq/CMakeLists.txt")
  add_subdirectory(with_zmq)
endif()

if(ZLINK_C_BUILD_BENCH_STREAMCOMPARE
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/with_stream/CMakeLists.txt")
  add_subdirectory(with_stream)
endif()

if(ZLINK_C_BUILD_BENCH_ROUTER_COMPARE
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/with_routing/CMakeLists.txt")
  add_subdirectory(with_routing)
endif()
