# with_routing - zlink vs libzmq vs gRPC server-to-server benchmark
#
# Compares performance of:
# - zlink ROUTER-ROUTER echo
# - libzmq ROUTER-ROUTER echo
# - gRPC bidirectional streaming echo
#
# Usage:
# 1. Copy libzmq to libzmq_dist/linux-x64/ (already included)
# 2. Run setup_grpc_dist.sh to populate grpc_dist/linux-x64/ or copy manually
# 3. Build with: cmake -B build -DBUILD_BENCHMARKS=ON && cmake --build build
# 4. Run: ./core/bench/with_routing/run_benchmarks.sh --runs 3

# --- zlink include ---
set(RC_ZLINK_INCLUDE_DIR "${ZLINK_C_BINDINGS_DIR}/include")

# --- libzmq (local dist inside libzmq/) ---
set(RC_LIBZMQ_PLATFORM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/libzmq_dist/linux-x64")
if(EXISTS "${RC_LIBZMQ_PLATFORM_ROOT}")
    set(RC_LIBZMQ_ROOT "${RC_LIBZMQ_PLATFORM_ROOT}")
else()
    set(RC_LIBZMQ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/libzmq_dist")
endif()
set(RC_LIBZMQ_INCLUDE_DIR "${RC_LIBZMQ_ROOT}/include")
file(GLOB RC_LIBZMQ_LIBRARY "${RC_LIBZMQ_ROOT}/lib/libzmq.so*")

# --- gRPC (local dist inside grpc/) ---
set(RC_GRPC_PLATFORM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/grpc/grpc_dist/linux-x64")
if(EXISTS "${RC_GRPC_PLATFORM_ROOT}")
    set(RC_GRPC_ROOT "${RC_GRPC_PLATFORM_ROOT}")
else()
    set(RC_GRPC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/grpc/grpc_dist")
endif()
set(RC_GRPC_INCLUDE_DIR "${RC_GRPC_ROOT}/include")
set(RC_GRPC_LIB_DIR "${RC_GRPC_ROOT}/lib")
set(RC_GRPC_BIN_DIR "${RC_GRPC_ROOT}/bin")
set(RC_PROTOC "${RC_GRPC_BIN_DIR}/protoc")
set(RC_GRPC_CPP_PLUGIN "${RC_GRPC_BIN_DIR}/grpc_cpp_plugin")

if(MSVC)
    set(RC_BENCH_CXX_FLAGS "/O2")
else()
    set(RC_BENCH_CXX_FLAGS "-O3")
endif()

option(ZLINK_BUILD_WITH_ROUTING_ZLINK_BENCHES
       "Build routing comparison benches through the C binding pull APIs"
       ON)

# ============================================================================
# zlink benchmarks (always build when headers exist)
# ============================================================================
if(EXISTS "${RC_ZLINK_INCLUDE_DIR}/zlink.h")
    if(ZLINK_BUILD_WITH_ROUTING_ZLINK_BENCHES)
        add_executable(bench_rc_zlink_server zlink/bench_zlink_router_server.cpp)
        target_link_libraries(bench_rc_zlink_server PRIVATE ${ZLINK_C_CORE_TARGET} Threads::Threads)
        target_include_directories(bench_rc_zlink_server PRIVATE
            ${RC_ZLINK_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/common)
        target_compile_options(bench_rc_zlink_server PRIVATE ${RC_BENCH_CXX_FLAGS})
        target_compile_features(bench_rc_zlink_server PRIVATE cxx_std_17)
        set_target_properties(bench_rc_zlink_server PROPERTIES
            BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}"
            INSTALL_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")

        add_executable(bench_rc_zlink_client zlink/bench_zlink_router_client.cpp)
        target_link_libraries(bench_rc_zlink_client PRIVATE ${ZLINK_C_CORE_TARGET} Threads::Threads)
        target_include_directories(bench_rc_zlink_client PRIVATE
            ${RC_ZLINK_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/common)
        target_compile_options(bench_rc_zlink_client PRIVATE ${RC_BENCH_CXX_FLAGS})
        target_compile_features(bench_rc_zlink_client PRIVATE cxx_std_17)
        set_target_properties(bench_rc_zlink_client PROPERTIES
            BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}"
            INSTALL_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")
    endif()
else()
    message(WARNING "zlink headers not found at ${RC_ZLINK_INCLUDE_DIR}")
endif()

# ============================================================================
# libzmq benchmarks (build when libzmq_dist exists)
# ============================================================================
if(EXISTS "${RC_LIBZMQ_INCLUDE_DIR}/zmq.h" AND RC_LIBZMQ_LIBRARY)
    message(STATUS "Found libzmq for router compare: ${RC_LIBZMQ_LIBRARY}")

    add_executable(bench_rc_zmq_server libzmq/bench_zmq_router_server.cpp)
    target_link_libraries(bench_rc_zmq_server PRIVATE ${RC_LIBZMQ_LIBRARY} Threads::Threads)
    target_include_directories(bench_rc_zmq_server PRIVATE
        ${RC_LIBZMQ_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_compile_options(bench_rc_zmq_server PRIVATE ${RC_BENCH_CXX_FLAGS})
    target_compile_features(bench_rc_zmq_server PRIVATE cxx_std_17)
    set_target_properties(bench_rc_zmq_server PROPERTIES
        BUILD_RPATH "${RC_LIBZMQ_ROOT}/lib"
        INSTALL_RPATH "${RC_LIBZMQ_ROOT}/lib")

    add_executable(bench_rc_zmq_client libzmq/bench_zmq_router_client.cpp)
    target_link_libraries(bench_rc_zmq_client PRIVATE ${RC_LIBZMQ_LIBRARY} Threads::Threads)
    target_include_directories(bench_rc_zmq_client PRIVATE
        ${RC_LIBZMQ_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_compile_options(bench_rc_zmq_client PRIVATE ${RC_BENCH_CXX_FLAGS})
    target_compile_features(bench_rc_zmq_client PRIVATE cxx_std_17)
    set_target_properties(bench_rc_zmq_client PROPERTIES
        BUILD_RPATH "${RC_LIBZMQ_ROOT}/lib"
        INSTALL_RPATH "${RC_LIBZMQ_ROOT}/lib")
else()
    message(STATUS "libzmq not found in ${RC_LIBZMQ_ROOT}. libzmq benchmarks will be skipped.")
endif()

# ============================================================================
# gRPC benchmarks (build when grpc_dist exists)
# ============================================================================
if(EXISTS "${RC_GRPC_INCLUDE_DIR}/grpcpp/grpcpp.h" AND EXISTS "${RC_PROTOC}" AND EXISTS "${RC_GRPC_CPP_PLUGIN}")
    message(STATUS "Found gRPC for router compare: ${RC_GRPC_ROOT}")

    # Resolve gRPC link libraries via pkg-config (handles abseil deps)
    find_package(PkgConfig QUIET)
    set(RC_GRPC_LINK_LIBS "")
    if(PkgConfig_FOUND)
        pkg_check_modules(RC_GRPCPP QUIET grpc++)
        pkg_check_modules(RC_PROTOBUF QUIET protobuf)
    endif()
    if(RC_GRPCPP_FOUND AND RC_PROTOBUF_FOUND)
        set(RC_GRPC_LINK_LIBS ${RC_GRPCPP_LIBRARIES} ${RC_PROTOBUF_LIBRARIES})
        message(STATUS "gRPC link libs from pkg-config: ${RC_GRPC_LINK_LIBS}")
    else()
        set(RC_GRPC_LINK_LIBS grpc++ grpc protobuf gpr address_sorting upb
            absl_synchronization absl_status absl_strings absl_str_format_internal
            absl_cord absl_base absl_spinlock_wait absl_time absl_time_zone
            absl_raw_logging_internal absl_throw_delegate absl_int128
            absl_stacktrace absl_symbolize absl_graphcycles_internal
            absl_malloc_internal absl_demangle_internal absl_debugging_internal
            re2 cares z ssl crypto)
        message(STATUS "gRPC link libs (fallback): core libs + abseil")
    endif()

    # Proto code generation
    set(RC_PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/echo.proto")
    set(RC_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
    file(MAKE_DIRECTORY ${RC_PROTO_GEN_DIR})

    set(RC_PROTO_CC "${RC_PROTO_GEN_DIR}/echo.pb.cc")
    set(RC_PROTO_H "${RC_PROTO_GEN_DIR}/echo.pb.h")
    set(RC_GRPC_CC "${RC_PROTO_GEN_DIR}/echo.grpc.pb.cc")
    set(RC_GRPC_H "${RC_PROTO_GEN_DIR}/echo.grpc.pb.h")

    add_custom_command(
        OUTPUT ${RC_PROTO_CC} ${RC_PROTO_H} ${RC_GRPC_CC} ${RC_GRPC_H}
        COMMAND ${RC_PROTOC}
            --cpp_out=${RC_PROTO_GEN_DIR}
            --grpc_out=${RC_PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${RC_GRPC_CPP_PLUGIN}
            -I${CMAKE_CURRENT_SOURCE_DIR}/proto
            ${RC_PROTO_SRC}
        DEPENDS ${RC_PROTO_SRC}
        COMMENT "Generating gRPC proto sources for router compare benchmark"
    )

    # gRPC server
    add_executable(bench_rc_grpc_server
        grpc/bench_grpc_echo_server.cpp
        ${RC_PROTO_CC}
        ${RC_GRPC_CC})
    target_include_directories(bench_rc_grpc_server PRIVATE
        ${RC_GRPC_INCLUDE_DIR}
        ${RC_PROTO_GEN_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_link_directories(bench_rc_grpc_server PRIVATE ${RC_GRPC_LIB_DIR})
    target_link_libraries(bench_rc_grpc_server PRIVATE
        ${RC_GRPC_LINK_LIBS} Threads::Threads)
    target_compile_options(bench_rc_grpc_server PRIVATE ${RC_BENCH_CXX_FLAGS})
    target_compile_features(bench_rc_grpc_server PRIVATE cxx_std_17)
    set_target_properties(bench_rc_grpc_server PROPERTIES
        BUILD_RPATH "${RC_GRPC_LIB_DIR}"
        INSTALL_RPATH "${RC_GRPC_LIB_DIR}")

    # gRPC client
    add_executable(bench_rc_grpc_client
        grpc/bench_grpc_echo_client.cpp
        ${RC_PROTO_CC}
        ${RC_GRPC_CC})
    target_include_directories(bench_rc_grpc_client PRIVATE
        ${RC_GRPC_INCLUDE_DIR}
        ${RC_PROTO_GEN_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_link_directories(bench_rc_grpc_client PRIVATE ${RC_GRPC_LIB_DIR})
    target_link_libraries(bench_rc_grpc_client PRIVATE
        ${RC_GRPC_LINK_LIBS} Threads::Threads)
    target_compile_options(bench_rc_grpc_client PRIVATE ${RC_BENCH_CXX_FLAGS})
    target_compile_features(bench_rc_grpc_client PRIVATE cxx_std_17)
    set_target_properties(bench_rc_grpc_client PROPERTIES
        BUILD_RPATH "${RC_GRPC_LIB_DIR}"
        INSTALL_RPATH "${RC_GRPC_LIB_DIR}")
else()
    message(STATUS "gRPC dist not found in ${RC_GRPC_ROOT}. gRPC benchmarks will be skipped. Run setup_grpc_dist.sh to populate.")
endif()
