cmake_minimum_required(VERSION 3.16)
project(zlink_memory_study CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Link against the prebuilt core library (core/build/lib).
set(ZLINK_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ZLINK_INCLUDE_DIR "${ZLINK_CORE_DIR}/include")
set(ZLINK_LIB_DIR "${ZLINK_CORE_DIR}/build/lib")

find_library(ZLINK_LIB zlink PATHS "${ZLINK_LIB_DIR}" NO_DEFAULT_PATH)
if(NOT ZLINK_LIB)
    message(FATAL_ERROR "libzlink not found in ${ZLINK_LIB_DIR}. Build core first (core/build.sh).")
endif()

add_library(study_common INTERFACE)
target_include_directories(study_common INTERFACE "${ZLINK_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(study_common INTERFACE "${ZLINK_LIB}" pthread)
# Make the runtime loader find libzlink.so.8 next to the build tree.
set(CMAKE_BUILD_RPATH "${ZLINK_LIB_DIR}")
set(CMAKE_INSTALL_RPATH "${ZLINK_LIB_DIR}")

foreach(prog
    srv_router
    srv_stream
    srv_pub
    srv_spot_hub
    cli_dealer_swarm
    cli_tcp_swarm
    cli_sub_swarm
    cli_spot_peers)
    add_executable(${prog} src/${prog}.cpp)
    target_link_libraries(${prog} PRIVATE study_common)
    set_target_properties(${prog} PROPERTIES BUILD_RPATH "${ZLINK_LIB_DIR}")
endforeach()
