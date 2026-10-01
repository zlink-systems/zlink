cmake_minimum_required(VERSION 3.24)

# Perf spec §6.5, §17.5: published builds use the sample bootstrap package; local builds stage the checkout Framework
# against the sample's local binding/Core packages before configuring this project.
project(ZlinkFrameworkPerf LANGUAGES C CXX)
include(CTest)
if(DEFINED ENV{ZLINK_PERF_FRAMEWORK_VERSION})
  set(ZLINK_PERF_FRAMEWORK_VERSION "$ENV{ZLINK_PERF_FRAMEWORK_VERSION}")
endif()
if(DEFINED ENV{ZLINK_PERF_PACKAGE_SOURCE})
  set(ZLINK_PERF_PACKAGE_SOURCE "$ENV{ZLINK_PERF_PACKAGE_SOURCE}")
endif()
if(NOT ZLINK_PERF_FRAMEWORK_VERSION)
  message(FATAL_ERROR "ZLINK_PERF_FRAMEWORK_VERSION is required; the common perf runner supplies the declared package version.")
endif()
if(NOT ZLINK_PERF_PACKAGE_SOURCE MATCHES "^(published|local)$")
  message(FATAL_ERROR "ZLINK_PERF_PACKAGE_SOURCE must be published or local.")
endif()
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
find_package(zlink CONFIG REQUIRED)
find_package(zlink_framework ${ZLINK_PERF_FRAMEWORK_VERSION} EXACT CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)
if(NOT zlink_cpp_VERSION)
  message(FATAL_ERROR "The selected Framework package did not expose its required zlink_cpp dependency version.")
endif()

# Header-only harness shared by every role and the client (§6.1 Shared, ServerSupport).
add_library(perf_shared INTERFACE)
target_include_directories(perf_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/shared")
target_compile_features(perf_shared INTERFACE cxx_std_20)
# One owner of the histogram bounds: the schema file the common runner also reads (§15.3).
target_compile_definitions(perf_shared INTERFACE
  PERF_HISTOGRAM_BOUNDS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../../../perf/schema/histogram-bounds.json"
  )
target_link_libraries(perf_shared INTERFACE zlink::framework zlink::framework_locations_redis
  zlink::stream_connector zlink::http_client)

# One executable per role directory (§17.5); the launcher maps a role name to perf_<directory>.
foreach(role IN ITEMS client session_actor_local_server session_server actor_server channel_server
                      spot_server actor_caller_server publisher_server subscriber_server)
  file(GLOB role_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${role}/*.cpp")
  if(role_sources)
    add_executable(perf_${role} ${role_sources})
    target_link_libraries(perf_${role} PRIVATE perf_shared)
    target_compile_features(perf_${role} PRIVATE cxx_std_20)
    set_target_properties(perf_${role} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(COMMAND zlink_framework_cpp_add_boost_headers)
      zlink_framework_cpp_add_boost_headers(perf_${role})
    else()
      target_link_libraries(perf_${role} PRIVATE Boost::headers)
    endif()
  endif()
endforeach()

if(BUILD_TESTING)
  add_executable(perf_contract_tests tests/perf_contract_tests.cpp)
  target_link_libraries(perf_contract_tests PRIVATE perf_shared)
  target_compile_features(perf_contract_tests PRIVATE cxx_std_20)
  if(COMMAND zlink_framework_cpp_add_boost_headers)
    zlink_framework_cpp_add_boost_headers(perf_contract_tests)
  else()
    target_link_libraries(perf_contract_tests PRIVATE Boost::headers)
  endif()
  add_test(NAME cpp_perf_contract_tests COMMAND perf_contract_tests)
  set_tests_properties(cpp_perf_contract_tests PROPERTIES LABELS "framework-unit")
endif()
