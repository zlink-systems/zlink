# Define standard libzmq location
if(WIN32)
    set(STD_LIBZMQ_PLATFORM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/libzmq_dist/windows-x64")
    if(EXISTS "${STD_LIBZMQ_PLATFORM_ROOT}")
        set(STD_LIBZMQ_ROOT "${STD_LIBZMQ_PLATFORM_ROOT}")
    else()
        set(STD_LIBZMQ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/libzmq_dist")
    endif()
else()
    set(STD_LIBZMQ_PLATFORM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/libzmq_dist/linux-x64")
    if(EXISTS "${STD_LIBZMQ_PLATFORM_ROOT}")
        set(STD_LIBZMQ_ROOT "${STD_LIBZMQ_PLATFORM_ROOT}")
    else()
        set(STD_LIBZMQ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/libzmq_dist")
    endif()
endif()

set(STD_LIBZMQ_INCLUDE_DIR "${STD_LIBZMQ_ROOT}/include")

if(WIN32)
    # Automatically find the .lib and .dll regardless of the versioned filename
    file(GLOB STD_LIBZMQ_LIBRARY "${STD_LIBZMQ_ROOT}/lib/*.lib")
    file(GLOB STD_LIBZMQ_DLL "${STD_LIBZMQ_ROOT}/bin/*.dll")
else()
    file(GLOB STD_LIBZMQ_LIBRARY "${STD_LIBZMQ_ROOT}/lib/libzmq.so*")
endif()

set(ZLINK_INCLUDE_DIR "${ZLINK_CORE_DIR}/include")
set(ZLINK_C_PERF_COMMON_DIR "${ZLINK_C_BINDINGS_DIR}/perf/common")
set(ZLINK_C_PERF_STREAMCLIENT_DIR
    "${ZLINK_C_BINDINGS_DIR}/perf/common/streamclient")
set(ZLINK_C_PERF_MULTI_COMMON_DIR "${ZLINK_C_BINDINGS_DIR}/perf/multi/common")

if(MSVC)
    set(BENCH_CXX_FLAGS "/O2")
else()
    set(BENCH_CXX_FLAGS "-O3")
endif()

option(ZLINK_BUILD_WITH_ZMQ_ZLINK_BENCHES
       "Build zlink-vs-zmq comparison benches that depend on synchronous recv APIs"
       OFF)

function(add_std_zmq_bench_single name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${STD_LIBZMQ_LIBRARY} Threads::Threads)
    target_include_directories(${name} PRIVATE ${STD_LIBZMQ_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/single/common)
    target_compile_options(${name} PRIVATE ${BENCH_CXX_FLAGS})
    target_compile_features(${name} PRIVATE cxx_std_17)
    
    if(WIN32)
        # Copy all runtime DLLs to the executable directory.
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STD_LIBZMQ_DLL}"
            "$<TARGET_FILE_DIR:${name}>"
        )
    else()
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH "${STD_LIBZMQ_ROOT}/lib"
            INSTALL_RPATH "${STD_LIBZMQ_ROOT}/lib"
        )
    endif()
endfunction()

function(add_std_zmq_bench_multi name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${STD_LIBZMQ_LIBRARY} Threads::Threads)
    if(TARGET OpenSSL::SSL)
        target_link_libraries(${name} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    target_include_directories(${name} PRIVATE
        ${ZLINK_C_PERF_COMMON_DIR}
        ${ZLINK_C_PERF_STREAMCLIENT_DIR}
        ${ZLINK_C_PERF_MULTI_COMMON_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/std_compat
        ${STD_LIBZMQ_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/multi/zmq
        ${CMAKE_CURRENT_SOURCE_DIR}/multi/common)
    target_compile_options(${name} PRIVATE ${BENCH_CXX_FLAGS})
    target_compile_definitions(${name} PRIVATE ZLINK_PERF_STD_COMPAT=1)
    target_compile_features(${name} PRIVATE cxx_std_17)
    
    if(WIN32)
        # Copy all runtime DLLs to the executable directory.
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STD_LIBZMQ_DLL}"
            "$<TARGET_FILE_DIR:${name}>"
        )
    else()
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH "${STD_LIBZMQ_ROOT}/lib"
            INSTALL_RPATH "${STD_LIBZMQ_ROOT}/lib"
        )
    endif()
endfunction()

function(add_zlink_bench_single name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${ZLINK_C_CORE_TARGET} Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(${name} PRIVATE dl)
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}"
            INSTALL_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")
    endif()
    target_include_directories(${name} PRIVATE ${ZLINK_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/single/common)
    target_compile_options(${name} PRIVATE ${BENCH_CXX_FLAGS})
    target_compile_features(${name} PRIVATE cxx_std_17)
    
    if(WIN32)
        # Copy the custom zlink.dll to the executable directory
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:libzlink>"
            "$<TARGET_FILE_DIR:${name}>/zlink.dll"
        )
    endif()
endfunction()

function(add_zlink_bench_multi name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${ZLINK_C_CORE_TARGET} Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(${name} PRIVATE dl)
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}"
            INSTALL_RPATH "${ZLINK_C_CORE_RUNTIME_DIR}")
    endif()
    target_include_directories(${name} PRIVATE
        ${ZLINK_C_PERF_COMMON_DIR}
        ${ZLINK_C_PERF_STREAMCLIENT_DIR}
        ${ZLINK_C_PERF_MULTI_COMMON_DIR}
        ${ZLINK_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/multi/zlink
        ${CMAKE_CURRENT_SOURCE_DIR}/multi/common)
    target_compile_options(${name} PRIVATE ${BENCH_CXX_FLAGS})
    target_compile_definitions(${name} PRIVATE ZLINK_PERF_STD_COMPAT=1)
    target_compile_features(${name} PRIVATE cxx_std_17)
    
    if(WIN32)
        # Copy the custom zlink.dll to the executable directory
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:libzlink>"
            "$<TARGET_FILE_DIR:${name}>/zlink.dll"
        )
    endif()
endfunction()

if(EXISTS "${ZLINK_INCLUDE_DIR}/zlink.h")
    if(ZLINK_BUILD_WITH_ZMQ_ZLINK_BENCHES)
        # --- zlink (current) ---
        add_zlink_bench_single(comp_zlink_pair single/zlink/bench_zlink_pair.cpp)
        add_zlink_bench_single(comp_zlink_pubsub single/zlink/bench_zlink_pubsub.cpp)
        add_zlink_bench_single(comp_zlink_dealer_dealer single/zlink/bench_zlink_dealer_dealer.cpp)
        add_zlink_bench_single(comp_zlink_dealer_router single/zlink/bench_zlink_dealer_router.cpp)
        add_zlink_bench_single(comp_zlink_router_router single/zlink/bench_zlink_router_router.cpp)
        add_zlink_bench_multi(comp_zlink_multi_dealer_dealer_client
                        multi/zlink/bench_zlink_multi_dealer_dealer_client.cpp)
        add_zlink_bench_multi(comp_zlink_multi_dealer_dealer_server
                        multi/zlink/bench_zlink_multi_dealer_dealer_server.cpp)
        add_zlink_bench_multi(comp_zlink_multi_dealer_router_client
                        multi/zlink/bench_zlink_multi_dealer_router_client.cpp)
        add_zlink_bench_multi(comp_zlink_multi_dealer_router_server
                        multi/zlink/bench_zlink_multi_dealer_router_server.cpp)
        add_zlink_bench_multi(comp_zlink_multi_router_router_client
                        multi/zlink/bench_zlink_multi_router_router_client.cpp)
        add_zlink_bench_multi(comp_zlink_multi_router_router_server
                        multi/zlink/bench_zlink_multi_router_router_server.cpp)
        add_zlink_bench_multi(comp_zlink_multi_pubsub_client
                        multi/zlink/bench_zlink_multi_pubsub_client.cpp)
        add_zlink_bench_multi(comp_zlink_multi_pubsub_server
                        multi/zlink/bench_zlink_multi_pubsub_server.cpp)
        add_zlink_bench_multi(comp_zlink_multi_stream_server
                        multi/zlink/bench_zlink_multi_stream_server.cpp)
        add_zlink_bench_multi(comp_zlink_multi_stream
                        multi/zlink/bench_zlink_multi_stream.cpp)
    else()
        message(STATUS
                "Skipping core/bench/with_zmq zlink comparison benches: required zlink comparison APIs are unavailable.")
    endif()
else()
    message(WARNING "zlink headers not found at ${ZLINK_INCLUDE_DIR}")
endif()

if(EXISTS "${STD_LIBZMQ_INCLUDE_DIR}/zmq.h" AND STD_LIBZMQ_LIBRARY)
    message(STATUS "Found standard libzmq for comparison: ${STD_LIBZMQ_LIBRARY}")

    # --- standard libzmq (baseline) ---
    add_std_zmq_bench_single(comp_std_zmq_pair single/zmq/bench_zmq_pair.cpp)
    add_std_zmq_bench_single(comp_std_zmq_pubsub single/zmq/bench_zmq_pubsub.cpp)
    add_std_zmq_bench_single(comp_std_zmq_dealer_dealer single/zmq/bench_zmq_dealer_dealer.cpp)
    add_std_zmq_bench_single(comp_std_zmq_dealer_router single/zmq/bench_zmq_dealer_router.cpp)
    add_std_zmq_bench_single(comp_std_zmq_router_router single/zmq/bench_zmq_router_router.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_stream
                      multi/zmq/bench_zmq_multi_stream.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_dealer_dealer_client
                      multi/zmq/bench_zmq_multi_dealer_dealer_client.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_dealer_dealer_server
                      multi/zmq/bench_zmq_multi_dealer_dealer_server.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_dealer_router_client
                      multi/zmq/bench_zmq_multi_dealer_router_client.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_dealer_router_server
                      multi/zmq/bench_zmq_multi_dealer_router_server.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_router_router_client
                      multi/zmq/bench_zmq_multi_router_router_client.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_router_router_server
                      multi/zmq/bench_zmq_multi_router_router_server.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_pubsub_client
                      multi/zmq/bench_zmq_multi_pubsub_client.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_pubsub_server
                      multi/zmq/bench_zmq_multi_pubsub_server.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_stream_server
                      multi/zmq/bench_zmq_multi_stream.cpp)
    add_std_zmq_bench_multi(comp_std_zmq_multi_stream_client
                      multi/zmq/bench_zmq_multi_stream_client.cpp)
else()
    message(STATUS "Standard libzmq not found. Comparative benchmarks will be skipped.")
endif()
