cmake_minimum_required(VERSION 3.24)

# Each sample is independently downloadable; this entry point owns its targets
# in both the workspace and installed-package builds.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_framework_root "${CMAKE_CURRENT_LIST_DIR}/../..")
  set(_package_default ON)
  if(EXISTS "${_framework_root}/CMakeLists.txt"
      AND EXISTS "${_framework_root}/framework/include/zlink/framework.hpp")
    set(_package_default OFF)
  endif()
  option(ZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE
    "Use installed framework packages instead of workspace sources" ${_package_default})
  # vcpkg runs during project(); select its manifest before loading the toolchain.
  if(NOT _package_default)
    list(APPEND VCPKG_OVERLAY_PORTS "${_framework_root}/../../../vcpkg/ports")
    if(NOT ZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE)
      set(VCPKG_MANIFEST_DIR "${_framework_root}")
    endif()
  endif()
  project(ZlinkSampleDeliveryDispatch LANGUAGES C CXX)
  set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
  if(ZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE)
    find_package(zlink CONFIG REQUIRED)
    find_package(zlink_framework 0.11.1 CONFIG REQUIRED)
  else()
    if(_package_default)
      message(FATAL_ERROR "Source mode requires this sample inside framework/languages/cpp/samples")
    endif()
    foreach(_option BUILD_SAMPLES BUILD_TESTS BUILD_FOUNDATION_TESTS BUILD_E2E BUILD_CROSS_LANGUAGE)
      set(ZLINK_FRAMEWORK_CPP_${_option} OFF CACHE BOOL "Standalone sample dependencies" FORCE)
    endforeach()
    add_subdirectory("${_framework_root}" "${CMAKE_CURRENT_BINARY_DIR}/framework" EXCLUDE_FROM_ALL)
  endif()
  message(STATUS "Zlink sample package mode: ${ZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE}")
endif()

add_executable(sample_cpp_framework_deliverydispatch_dispatch Server/Dispatch/main.cpp)
target_link_libraries(sample_cpp_framework_deliverydispatch_dispatch PRIVATE
  zlink::framework zlink::framework_locations_redis zlink::http_client)

add_executable(sample_cpp_framework_deliverydispatch_courier_actor_node Server/CourierActorNode/main.cpp)
target_link_libraries(sample_cpp_framework_deliverydispatch_courier_actor_node PRIVATE
  zlink::framework zlink::framework_locations_redis)

add_executable(sample_cpp_framework_deliverydispatch_customer_gateway Server/CustomerGateway/main.cpp)
target_link_libraries(sample_cpp_framework_deliverydispatch_customer_gateway PRIVATE
  zlink::framework zlink::framework_locations_redis)

add_executable(sample_cpp_framework_deliverydispatch_courier_session Server/CourierSession/main.cpp)
target_link_libraries(sample_cpp_framework_deliverydispatch_courier_session PRIVATE
  zlink::framework zlink::framework_locations_redis)

add_executable(sample_cpp_framework_deliverydispatch_tracking Server/Tracking/main.cpp)
target_link_libraries(sample_cpp_framework_deliverydispatch_tracking PRIVATE
  zlink::framework zlink::framework_locations_redis)

add_executable(sample_cpp_framework_deliverydispatch_client Client/main.cpp)
target_link_libraries(sample_cpp_framework_deliverydispatch_client PRIVATE
  zlink::framework zlink::http_client zlink::stream_connector zlink::stream_e2e_client)

get_property(_sample_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _sample_targets)
  target_compile_features(${_target} PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(${_target} PRIVATE /bigobj)
  endif()
  set_target_properties(${_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(COMMAND zlink_framework_cpp_add_boost_headers)
    zlink_framework_cpp_add_boost_headers(${_target})
  else()
    target_link_libraries(${_target} PRIVATE Boost::headers)
  endif()
endforeach()
