cmake_minimum_required(VERSION 3.16)
project(zlink_hwm_bytes_study CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Link against the official prebuilt core runtime (core/build/lib).
set(ZLINK_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ZLINK_INCLUDE_DIR "${ZLINK_CORE_DIR}/include")
set(ZLINK_LIB_DIR "${ZLINK_CORE_DIR}/build/lib")

find_library(ZLINK_LIB zlink PATHS "${ZLINK_LIB_DIR}" NO_DEFAULT_PATH)
if(NOT ZLINK_LIB)
    message(FATAL_ERROR "libzlink not found in ${ZLINK_LIB_DIR}. Build core first (cmake --build core/build).")
endif()

add_executable(hwm_memory_amplification hwm_memory_amplification.cpp)
target_include_directories(hwm_memory_amplification PRIVATE "${ZLINK_INCLUDE_DIR}")
target_link_libraries(hwm_memory_amplification PRIVATE "${ZLINK_LIB}" pthread)
set_target_properties(hwm_memory_amplification PROPERTIES
    BUILD_RPATH "${ZLINK_LIB_DIR}"
    INSTALL_RPATH "${ZLINK_LIB_DIR}")
