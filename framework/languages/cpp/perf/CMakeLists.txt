cmake_minimum_required(VERSION 3.24)

# Perf spec §6.5, §17.5: the perf project consumes the published framework-cpp prebuilt package only.
# scripts/build_role.sh runs samples/bootstrap.cmake with -DZLINK_PROJECT_DIR=<this directory> once: it downloads and
# extracts the release package and configures this project against it, exactly as a sample in package mode.
# There is no workspace mode: perf never links Core or Framework sources.
project(ZlinkFrameworkPerf LANGUAGES C CXX)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
find_package(zlink CONFIG REQUIRED)
find_package(zlink_framework CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)

# Header-only harness shared by every role and the client (§6.1 Shared, ServerSupport).
add_library(perf_shared INTERFACE)
target_include_directories(perf_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/shared")
target_compile_features(perf_shared INTERFACE cxx_std_20)
# One owner of the histogram bounds: the schema file the common runner also reads (§15.3).
target_compile_definitions(perf_shared INTERFACE
  PERF_HISTOGRAM_BOUNDS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../../../perf/schema/histogram-bounds.json")
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
