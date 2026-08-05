find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

option(ZLINK_C_BUILD_BENCH_ZMQ
       "Build bindings/c/bench/with_zmq targets"
       ${ZLINK_C_BUILD_BENCHES})
option(ZLINK_C_BUILD_BENCH_STREAMCOMPARE
       "Build bindings/c/bench/with_stream targets"
       ${ZLINK_C_BUILD_BENCHES})
option(ZLINK_C_BUILD_BENCH_ROUTER_COMPARE
       "Build bindings/c/bench/with_routing targets"
       ${ZLINK_C_BUILD_BENCHES})
option(ZLINK_C_BUILD_BENCH_GRPC_COMPARE
       "Build bindings/c/bench/with_grpc targets"
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

if(ZLINK_C_BUILD_BENCH_GRPC_COMPARE
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/with_grpc/CMakeLists.txt")
  add_subdirectory(with_grpc)
endif()
