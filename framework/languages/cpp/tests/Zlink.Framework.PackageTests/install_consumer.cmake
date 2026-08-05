if(NOT DEFINED ZLINK_FRAMEWORK_CPP_BUILD_DIR)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_BUILD_DIR is required")
endif()
if(NOT DEFINED ZLINK_FRAMEWORK_CPP_INSTALL_PREFIX)
  message(FATAL_ERROR "ZLINK_FRAMEWORK_CPP_INSTALL_PREFIX is required")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef consumer_run_id)
set(consumer_run_dir
  "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}/package-consumer-runs/${consumer_run_id}")
set(consumer_install_prefix
  "${consumer_run_dir}/install")
set(consumer_source_dir
  "${consumer_run_dir}/src")
set(consumer_build_dir
  "${consumer_run_dir}/build")

file(MAKE_DIRECTORY "${consumer_run_dir}")
file(MAKE_DIRECTORY "${consumer_source_dir}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${ZLINK_FRAMEWORK_CPP_BUILD_DIR}"
          --prefix "${consumer_install_prefix}"
  RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "C++ framework install failed")
endif()

set(framework_targets_file
  "${consumer_install_prefix}/lib/cmake/zlink_framework_cpp/zlink_framework_cppTargets.cmake")
set(connector_targets_file
  "${consumer_install_prefix}/lib/cmake/zlink_stream_connector_cpp/zlink_stream_connector_cppTargets.cmake")
set(framework_config_file
  "${consumer_install_prefix}/lib/cmake/zlink_framework_cpp/zlink_framework_cppConfig.cmake")
set(connector_config_file
  "${consumer_install_prefix}/lib/cmake/zlink_stream_connector_cpp/zlink_stream_connector_cppConfig.cmake")
foreach(path IN ITEMS
    "${framework_config_file}"
    "${connector_config_file}"
    "${framework_targets_file}"
    "${connector_targets_file}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "installed package file is missing: ${path}")
  endif()
endforeach()
file(READ "${framework_config_file}" framework_config_text)
file(READ "${connector_config_file}" connector_config_text)
file(READ "${framework_targets_file}" framework_targets_text)
file(READ "${connector_targets_file}" connector_targets_text)
foreach(required_text IN ITEMS
    "find_dependency(Threads)"
    "find_dependency(nlohmann_json CONFIG)"
    "../zlink_cpp/zlink_cppTargets.cmake"
    "zlink_framework_cppTargets.cmake")
  string(FIND "${framework_config_text}" "${required_text}" required_pos)
  if(required_pos EQUAL -1)
    message(FATAL_ERROR "framework package config lacks ${required_text}")
  endif()
endforeach()
if(ZLINK_FRAMEWORK_CPP_EXPECT_REDIS_PLUS_PLUS)
  string(FIND "${framework_config_text}"
    "find_dependency(redis++ CONFIG)" dependency_pos)
  if(dependency_pos EQUAL -1)
    message(FATAL_ERROR "framework package config lacks redis++ dependency")
  endif()
endif()
if(ZLINK_FRAMEWORK_CPP_EXPECT_LIBUV)
  string(FIND "${framework_config_text}"
    "find_dependency(libuv CONFIG)" dependency_pos)
  if(dependency_pos EQUAL -1)
    message(FATAL_ERROR "framework package config lacks libuv dependency")
  endif()
endif()
foreach(forbidden_text IN ITEMS
    "zlink_stream_connector_cppTargets.cmake"
    "find_dependency(msgpack-cxx CONFIG)"
    "find_dependency(Protobuf)")
  string(FIND "${framework_config_text}" "${forbidden_text}" forbidden_pos)
  if(NOT forbidden_pos EQUAL -1)
    message(FATAL_ERROR "framework package config must not include ${forbidden_text}")
  endif()
endforeach()
foreach(required_text IN ITEMS
    "find_dependency(Threads)"
    "find_dependency(nlohmann_json)"
    "../zlink_cpp/zlink_cppTargets.cmake"
    "zlink_stream_connector_cppTargets.cmake")
  string(FIND "${connector_config_text}" "${required_text}" required_pos)
  if(required_pos EQUAL -1)
    message(FATAL_ERROR "stream connector package config lacks ${required_text}")
  endif()
endforeach()
foreach(forbidden_text IN ITEMS
    "zlink_framework_cppTargets.cmake")
  string(FIND "${connector_config_text}" "${forbidden_text}" forbidden_pos)
  if(NOT forbidden_pos EQUAL -1)
    message(FATAL_ERROR "stream connector package config must not include ${forbidden_text}")
  endif()
endforeach()
foreach(required_target IN ITEMS
    "zlink::framework")
  if(NOT framework_targets_text MATCHES "${required_target}")
    message(FATAL_ERROR "framework package export lacks ${required_target}")
  endif()
endforeach()
set(http_client_targets_file
  "${consumer_install_prefix}/lib/cmake/zlink_http_client_cpp/zlink_http_client_cppTargets.cmake")
set(http_client_config_file
  "${consumer_install_prefix}/lib/cmake/zlink_http_client_cpp/zlink_http_client_cppConfig.cmake")
foreach(path IN ITEMS "${http_client_targets_file}" "${http_client_config_file}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "installed http client package file is missing: ${path}")
  endif()
endforeach()
file(READ "${http_client_targets_file}" http_client_targets_text)
if(NOT http_client_targets_text MATCHES "zlink::http_client")
  message(FATAL_ERROR "http client package export lacks zlink::http_client")
endif()
if(framework_targets_text MATCHES "zlink::http_client")
  message(FATAL_ERROR "framework package export must not include zlink::http_client")
endif()
foreach(required_target IN ITEMS
    "zlink::stream_connector"
    "zlink::stream_connector_codecs"
    "zlink::stream_e2e_client"
    "zlink::stream_connector_throwing")
  if(NOT connector_targets_text MATCHES "${required_target}")
    message(FATAL_ERROR "stream connector package export lacks ${required_target}")
  endif()
endforeach()

file(WRITE "${consumer_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(zlink_framework_cpp_consumer LANGUAGES CXX)

find_package(zlink_framework_cpp CONFIG REQUIRED)
find_package(zlink_http_client_cpp CONFIG REQUIRED)
find_package(zlink_stream_connector_cpp CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE
  zlink::framework_provider_abstractions
  zlink::framework
  zlink::framework_locations_redis
  zlink::http_client
  zlink::stream_connector
  zlink::stream_connector_codecs
  zlink::stream_e2e_client
  zlink::stream_connector_throwing)
]=])

file(WRITE "${consumer_source_dir}/main.cpp" [=[
#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>
#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_connector_throwing.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <nlohmann/json.hpp>

struct login_request_t
{
  static constexpr const char *packet_name = "LoginRequest";
};

void
to_json (nlohmann::json &json, const login_request_t &)
{
  json = nlohmann::json::object ();
}

int
main ()
{
  auto app = zlink::framework::app_t::create ();
  (void) app;
  auto redis_options =
    zlink::framework::redis::redis_location_options_t{
      .connection_string = "tcp://127.0.0.1:6379",
      .key_prefix = "zlink:package-test:location"};
  auto relocation_options =
    zlink::framework::redis::redis_relocation_options_t{
      .connection_string = "tcp://127.0.0.1:6379",
      .key_prefix = "zlink:package-test:relocation"};
  auto location_store =
    zlink::framework::redis::redis_location_store_t(redis_options);
  auto relocation_store =
    zlink::framework::redis::redis_relocation_store_t(relocation_options);
  (void) location_store;
  (void) relocation_store;
  auto client = zlink::http_client::client_t::create ()
                  .base_url ("http://127.0.0.1:18080")
                  .build ();
  (void) client;
  auto packet =
    zlink::stream_connector::codecs::encode_packet (login_request_t {});
  auto connector =
    zlink::stream_connector::connector_factory_t::create (
      zlink::stream_connector::connector_options_t{});
  auto e2e_client = zlink::stream_e2e_client::use (connector);
  (void) e2e_client;
  auto throwing_connector =
    zlink::stream_connector_throwing::create (
      zlink::stream_connector::connector_options_t{});
  (void) throwing_connector;
  return packet.name == login_request_t::packet_name ? 0 : 1;
}
]=])

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${consumer_source_dir}" -B "${consumer_build_dir}"
          "-DCMAKE_PREFIX_PATH=${consumer_install_prefix};${ZLINK_FRAMEWORK_CPP_DEPENDENCY_PREFIX_PATH}"
  RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "installed C++ framework consumer configure failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build_dir}"
  RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "installed C++ framework consumer build failed")
endif()

if(WIN32)
  set(runtime_path
    "PATH=${consumer_install_prefix}/bin;$ENV{PATH}")
elseif(APPLE)
  set(runtime_path
    "DYLD_LIBRARY_PATH=${consumer_install_prefix}/lib:$ENV{DYLD_LIBRARY_PATH}")
else()
  set(runtime_path
    "LD_LIBRARY_PATH=${consumer_install_prefix}/lib:$ENV{LD_LIBRARY_PATH}")
endif()

set(consumer_gcov_prefix "${consumer_run_dir}/gcov")
file(MAKE_DIRECTORY "${consumer_gcov_prefix}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "${runtime_path}"
          "GCOV_PREFIX=${consumer_gcov_prefix}"
          "GCOV_PREFIX_STRIP=0"
          "${consumer_build_dir}/consumer"
  RESULT_VARIABLE run_result)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "installed C++ framework consumer run failed")
endif()
