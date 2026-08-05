cmake_minimum_required(VERSION 3.20)
project(zlink_framework_cpp LANGUAGES C CXX)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility=hidden>
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-fno-gnu-unique>)
  add_link_options($<$<LINK_LANGUAGE:CXX>:-Wl,--exclude-libs,ALL>)
  if(UNIX AND NOT APPLE AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/cmake/hide-executable-symbols.map")
    add_link_options(
      $<$<LINK_LANGUAGE:CXX>:-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/cmake/hide-executable-symbols.map>)
  endif()
endif()

include(GNUInstallDirs)
include(FetchContent)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(ZLINK_FRAMEWORK_CPP_BUILD_TESTS "Build C++ framework tests" ON)
option(ZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS "Build isolated C++ service foundation tests" ON)
option(ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES "Build C++ framework samples" ON)
option(ZLINK_FRAMEWORK_CPP_BUILD_E2E "Build C++ framework scenario e2e apps" ON)
option(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK "Install C++ framework and HTTP client package artifacts" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT "Build Stream Connector e2e client helpers" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_UNREAL "Build Unreal Stream Connector source adapter" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_GODOT "Build Godot Stream Connector source adapter" ON)
option(ZLINK_STREAM_CONNECTOR_BUILD_AXMOL "Build Axmol Stream Connector source adapter" ON)
option(ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4 "Enable C++ framework STREAM LZ4 compression" ON)
option(ZLINK_STREAM_CONNECTOR_WITH_LZ4 "Enable Stream Connector LZ4 compression" ON)
option(ZLINK_STREAM_CONNECTOR_WITH_TLS "Enable Stream Connector TLS transport" ON)
option(ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET "Enable Stream Connector WebSocket transport" ON)
option(ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE "Enable C++ framework gcov coverage instrumentation" OFF)
option(ZLINK_FRAMEWORK_CPP_REQUIRE_HTTP_PERF_REPORT
  "Register the external HTTP performance comparison as a required CTest gate" OFF)
set(ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD 80 CACHE STRING
  "Minimum C++ framework runtime line coverage percentage")

set(ZLINK_FRAMEWORK_CPP_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(ZLINK_FRAMEWORK_CPP_REPO_ROOT "${ZLINK_FRAMEWORK_CPP_DIR}/../../.." CACHE PATH
  "Path to the zlink repository root")
set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT "$ENV{ZLINK_LOCAL_PACKAGE_ROOT}" CACHE PATH
  "Root directory containing local package outputs")
set(ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION "11.2.0" CACHE STRING
  "Exact local zlink_cpp package version consumed by the C++ framework")
set(ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION "11.2.0" CACHE STRING
  "Exact local zlink Core package version embedded by zlink_cpp")
if(NOT ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT)
  if(WIN32 AND EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/.artifacts/windows/install/zlink-cpp/${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION}")
    set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT
      "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/.artifacts/windows")
  else()
    set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT
      "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/.artifacts/wsl")
  endif()
endif()
set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX
  "${ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT}/install/zlink-cpp/${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION}" CACHE PATH
  "Local zlink_cpp package install prefix")
set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX
  "${ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT}/install/zlink-core/${ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION}" CACHE PATH
  "Local zlink Core package install prefix used by zlink_cpp")
if(WIN32)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/bin/zlink.dll")
  else()
    set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/bin/zlink.dll")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.dylib")
  else()
    set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.dylib")
  endif()
else()
  set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.so")
endif()

if(NOT TARGET zlink::cpp)
  list(PREPEND CMAKE_PREFIX_PATH
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}"
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}")
  find_package(zlink_cpp ${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION} EXACT CONFIG REQUIRED)
endif()

add_library(zlink_framework_provider_abstractions INTERFACE)
add_library(zlink::framework_provider_abstractions ALIAS
  zlink_framework_provider_abstractions)
set_target_properties(zlink_framework_provider_abstractions PROPERTIES
  EXPORT_NAME framework_provider_abstractions)
target_compile_features(zlink_framework_provider_abstractions INTERFACE cxx_std_20)
target_include_directories(zlink_framework_provider_abstractions INTERFACE
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/framework/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set(ZLINK_FRAMEWORK_CPP_M6A_RUNTIME_SOURCES
  framework/src/runtime/backend/native_route_backend.cpp
  framework/src/runtime/backend/raw_dealer_port.cpp
  framework/src/runtime/backend/raw_route_port.cpp
  framework/src/runtime/foundation/operation_registry.cpp
  framework/src/runtime/protocol/service_wire_codec.cpp
  framework/src/runtime/mesh/raw_mesh_node_owner.cpp
  framework/src/runtime/mesh/service_liveness_registry.cpp
  framework/src/runtime/mesh/service_mailbox.cpp
  framework/src/runtime/mesh/service_topology_registry.cpp
  framework/src/runtime/locations/service_descriptor_registry.cpp
  framework/src/runtime/fanout/raw_fanout_owner.cpp
  framework/src/runtime/fanout/fanout_location_runtime.cpp
  framework/src/runtime/client_server/raw_client_server_owner.cpp
  framework/src/runtime/client_server/client_server_location_runtime.cpp)

set(ZLINK_FRAMEWORK_CPP_M6B_RUNTIME_SOURCES
  framework/src/runtime/stateful/public_host_runtime.cpp
  framework/src/runtime/stateful/raw_stateful_dispatch.cpp
  framework/src/runtime/stateful/stateful_object_runtime.cpp
  framework/src/runtime/stateful/stream_session_registry.cpp)

set(ZLINK_FRAMEWORK_CPP_M6C_RUNTIME_SOURCES
  framework/src/runtime/stateful/maintenance_runtime.cpp)

add_library(zlink_framework_m6a_runtime STATIC
  ${ZLINK_FRAMEWORK_CPP_M6A_RUNTIME_SOURCES})
target_compile_features(zlink_framework_m6a_runtime PUBLIC cxx_std_20)
target_include_directories(zlink_framework_m6a_runtime PUBLIC
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include)
target_include_directories(zlink_framework_m6a_runtime PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
  ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
target_link_libraries(zlink_framework_m6a_runtime PUBLIC
  zlink::cpp
  Threads::Threads)

add_library(zlink_framework_m6b_runtime STATIC
  ${ZLINK_FRAMEWORK_CPP_M6B_RUNTIME_SOURCES})
target_compile_features(zlink_framework_m6b_runtime PUBLIC cxx_std_20)
target_include_directories(zlink_framework_m6b_runtime PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
  ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
target_link_libraries(zlink_framework_m6b_runtime PUBLIC
  zlink_framework_m6a_runtime
  Threads::Threads)

add_library(zlink_framework_m6c_runtime STATIC
  ${ZLINK_FRAMEWORK_CPP_M6C_RUNTIME_SOURCES})
target_compile_features(zlink_framework_m6c_runtime PUBLIC cxx_std_20)
target_include_directories(zlink_framework_m6c_runtime PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
  ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
target_link_libraries(zlink_framework_m6c_runtime PUBLIC
  zlink_framework_m6b_runtime
  Threads::Threads)

add_library(zlink_framework STATIC
  ${ZLINK_FRAMEWORK_CPP_M6A_RUNTIME_SOURCES}
  ${ZLINK_FRAMEWORK_CPP_M6B_RUNTIME_SOURCES}
  ${ZLINK_FRAMEWORK_CPP_M6C_RUNTIME_SOURCES}
  framework/src/runtime/channels/channel_packet_dispatcher.cpp
  framework/src/runtime/channels/channel_host_service.cpp
  framework/src/runtime/channels/channel_bundle_factory.cpp
  framework/src/runtime/channels/channel_pending_requests.cpp
  framework/src/runtime/channels/channel_reply_writer.cpp
  framework/src/runtime/channels/channel_outbound_exchange.cpp
  framework/src/runtime/channels/channel_runtime_bundle.cpp
  framework/src/runtime/channels/channel_runtime_manager.cpp
  framework/src/runtime/channels/channel_runtime.cpp
  framework/src/runtime/channels/route_channel_registration.cpp
  framework/src/runtime/channels/route_channel_runtime.cpp
  framework/src/runtime/channels/route_connection_set.cpp
  framework/src/runtime/channels/route_handler_invoker.cpp
  framework/src/runtime/channels/route_handler_registry.cpp
  framework/src/runtime/channels/route_internal_packet_dispatcher.cpp
  framework/src/runtime/channels/route_packet_dispatcher.cpp
  framework/src/runtime/actors/actor_route_internal_dispatcher.cpp
  framework/src/runtime/actors/actor_gateway_runtime.cpp
  framework/src/runtime/actors/actor_client.cpp
  framework/src/runtime/configuration/builders/configuration_builder.cpp
  framework/src/runtime/configuration/endpoint_connections.cpp
  framework/src/runtime/configuration/services.cpp
  framework/src/runtime/codecs/serializer.cpp
  framework/src/runtime/diagnostics/logging.cpp
  framework/src/runtime/diagnostics/health.cpp
  framework/src/runtime/diagnostics/monitoring_runtime.cpp
  framework/src/runtime/dispatch/coroutine_executor.cpp
  framework/src/runtime/dispatch/offload_executor.cpp
  framework/src/runtime/execution/serial_execution_queue.cpp
  framework/src/runtime/handlers/handler_registry.cpp
  framework/src/runtime/dispatch/projection.cpp
  framework/src/runtime/host/app.cpp
  framework/src/runtime/diagnostics/flow_context.cpp
  framework/src/runtime/http/http_listener.cpp
  framework/src/runtime/http/http_request_pipeline.cpp
  framework/src/runtime/host/framework_runtime.cpp
  framework/src/runtime/messaging/client_call_codec.cpp
  framework/src/runtime/messaging/envelope_codec.cpp
  framework/src/runtime/messaging/pending_operation.cpp
  framework/src/runtime/messaging/pending_submit.cpp
  framework/src/runtime/messaging/async_submit_runtime.cpp
  framework/src/runtime/messaging/request_failure_mapper.cpp
  framework/src/runtime/messaging/submit_queue.cpp
  framework/src/runtime/mesh/mesh_metadata_codec.cpp
  framework/src/runtime/mesh/mesh_node_host_service.cpp
  framework/src/runtime/mesh/mesh_node_runtime.cpp
  framework/src/runtime/mesh/route_mesh_runtime_service.cpp
  framework/src/runtime/mesh/route_mesh_runtime_options_service.cpp
  framework/src/runtime/spots/spot_runtime.cpp
  framework/src/runtime/mesh/mesh_record_dispatcher.cpp
  framework/src/runtime/spots/actor_transfer_coordinator.cpp
  framework/src/runtime/spots/spot_ids.cpp
  framework/src/runtime/spots/spot_route_internal_dispatcher.cpp
  framework/src/runtime/spots/spot_route_packets.cpp
  framework/src/runtime/streams/stream_compression.cpp
  framework/src/runtime/streams/stream_runtime.cpp
  framework/src/runtime/streams/stream_host_service.cpp
  framework/src/runtime/timers/timer_runtime.cpp
  framework/src/runtime/transport/endpoint.cpp)
add_library(zlink::framework ALIAS zlink_framework)
set_target_properties(zlink_framework PROPERTIES EXPORT_NAME framework)
target_compile_features(zlink_framework PUBLIC cxx_std_20)
target_include_directories(zlink_framework PUBLIC
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/framework/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(zlink_framework PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
  ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
target_link_libraries(zlink_framework PUBLIC zlink::cpp)
set(ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR "" CACHE PATH
  "Boost include directory used by the C++ framework and stream connector")
if(NOT ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR
    AND EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/external/boost/boost/asio.hpp")
  set(ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR
    "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/external/boost" CACHE PATH
    "Boost include directory used by the C++ framework and stream connector" FORCE)
endif()
find_package(Boost QUIET)
function(zlink_framework_cpp_add_boost_headers target_name)
  if(ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR)
    # Keep the bundled Boost tree ahead of transitive package include paths. A
    # framework target and the standalone HTTP client are linked into the same
    # consumer; selecting different Asio headers in those targets is an ABI
    # violation even though the headers are nominally header-only.
    target_include_directories(${target_name} BEFORE PRIVATE
      "${ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR}")
  elseif(Boost_FOUND AND Boost_INCLUDE_DIRS)
    target_include_directories(${target_name} BEFORE PRIVATE ${Boost_INCLUDE_DIRS})
  elseif(Boost_FOUND AND Boost_INCLUDE_DIR)
    target_include_directories(${target_name} BEFORE PRIVATE ${Boost_INCLUDE_DIR})
  else()
    message(FATAL_ERROR
      "Boost headers were not found. Install Boost, configure with vcpkg, "
      "or make sure core/external/boost exists in the repository checkout.")
  endif()
endfunction()
zlink_framework_cpp_add_boost_headers(zlink_framework)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()
find_package(OpenSSL QUIET)
target_link_libraries(zlink_framework PUBLIC nlohmann_json::nlohmann_json)
if(OpenSSL_FOUND)
  target_compile_definitions(zlink_framework PRIVATE
    ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
    ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL)
  target_link_libraries(zlink_framework PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

add_library(zlink_http_client STATIC
  http-client/src/client.cpp
  http-client/src/runtime/compression.cpp
  http-client/src/runtime/connection_opener.cpp
  http-client/src/runtime/connection_pool.cpp
  http-client/src/runtime/cookie_jar.cpp
  http-client/src/runtime/coroutine_scheduler.cpp
  http-client/src/runtime/request_performer.cpp
  http-client/src/runtime/retry_policy.cpp
  http-client/src/runtime/runtime_errors.cpp
  http-client/src/runtime/http_client_runtime.cpp
  http-client/src/runtime/text.cpp
  http-client/src/runtime/url.cpp)
add_library(zlink::http_client ALIAS zlink_http_client)
set_target_properties(zlink_http_client PROPERTIES EXPORT_NAME http_client)
target_compile_features(zlink_http_client PUBLIC cxx_std_20)
target_include_directories(zlink_http_client PUBLIC
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/http-client/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(zlink_http_client PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/http-client/src)
target_link_libraries(zlink_http_client PUBLIC
  zlink::framework
  nlohmann_json::nlohmann_json)
target_link_libraries(zlink_http_client PRIVATE Threads::Threads)
# All Boost consumers must resolve the same Boost checkout. Mixing Boost trees
# in one binary (for example vendored asio with a system beast) is undefined
# behavior at the ABI level, so the http client uses the shared helper instead
# of its own include wiring.
zlink_framework_cpp_add_boost_headers(zlink_http_client)
if(OpenSSL_FOUND)
  target_compile_definitions(zlink_http_client PRIVATE
    ZLINK_HTTP_CLIENT_WITH_OPENSSL)
  target_link_libraries(zlink_http_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  set(ZLINK_HTTP_CLIENT_EXPORT_OPENSSL_DEPENDENCY
    "find_dependency(OpenSSL)")
else()
  set(ZLINK_HTTP_CLIENT_EXPORT_OPENSSL_DEPENDENCY "")
endif()
find_program(ZLINK_OPENSSL_EXECUTABLE openssl)
if(OpenSSL_FOUND AND ZLINK_OPENSSL_EXECUTABLE)
  set(ZLINK_HTTP_CLIENT_TEST_CERT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/http-client-test-certs")
  set(ZLINK_HTTP_CLIENT_TEST_CERT
    "${ZLINK_HTTP_CLIENT_TEST_CERT_DIR}/server.crt")
  set(ZLINK_HTTP_CLIENT_TEST_KEY
    "${ZLINK_HTTP_CLIENT_TEST_CERT_DIR}/server.key")
  file(MAKE_DIRECTORY "${ZLINK_HTTP_CLIENT_TEST_CERT_DIR}")
  execute_process(
    COMMAND ${ZLINK_OPENSSL_EXECUTABLE}
      req -x509 -newkey rsa:2048 -nodes -days 7
      -subj /CN=localhost
      -addext subjectAltName=DNS:localhost
      -keyout "${ZLINK_HTTP_CLIENT_TEST_KEY}"
      -out "${ZLINK_HTTP_CLIENT_TEST_CERT}"
    RESULT_VARIABLE ZLINK_HTTP_CLIENT_CERT_RESULT
    OUTPUT_QUIET
    ERROR_QUIET)
  if(NOT ZLINK_HTTP_CLIENT_CERT_RESULT EQUAL 0)
    message(WARNING
      "Could not generate HTTP client HTTPS test certificate; HTTPS e2e test will be disabled.")
  endif()
  if(EXISTS "${ZLINK_HTTP_CLIENT_TEST_CERT}" AND
      EXISTS "${ZLINK_HTTP_CLIENT_TEST_KEY}")
    set(ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL ON)
  endif()
endif()

add_library(zlink_stream_connector STATIC
  connector/core/src/runtime/calls/zlink_stream_calls.cpp
  connector/core/src/runtime/connector_runtime.cpp
  connector/core/src/runtime/protocol/compression/lz4_compression_codec.cpp
  connector/core/src/runtime/protocol/framing/frame_codec.cpp
  connector/core/src/runtime/protocol/framing.cpp
  connector/core/src/runtime/protocol/header_codec.cpp
  connector/core/src/runtime/protocol/metadata_codec.cpp
  connector/core/src/runtime/protocol/packet_name_resolver.cpp
  connector/core/src/runtime/transport/stream_connection.cpp
  connector/core/src/runtime/transport/stream_transport_factory.cpp
  connector/core/src/runtime/transport/websocket_connection.cpp)
add_library(zlink::stream_connector ALIAS zlink_stream_connector)
set_target_properties(zlink_stream_connector PROPERTIES
  EXPORT_NAME stream_connector)
target_compile_features(zlink_stream_connector PUBLIC cxx_std_20)
target_include_directories(zlink_stream_connector PUBLIC
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(zlink_stream_connector PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/src)
zlink_framework_cpp_add_boost_headers(zlink_stream_connector)
target_link_libraries(zlink_stream_connector PUBLIC zlink::cpp)
target_link_libraries(zlink_stream_connector PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(ZLINK_STREAM_CONNECTOR_WITH_TLS AND OpenSSL_FOUND)
  target_compile_definitions(zlink_stream_connector PRIVATE
    ZLINK_STREAM_CONNECTOR_WITH_OPENSSL)
  target_link_libraries(zlink_stream_connector PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto)
  set(ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY
    "find_dependency(OpenSSL)")
else()
  set(ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY "")
endif()
if(ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET)
  target_compile_definitions(zlink_stream_connector PRIVATE
    ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET)
endif()
if(ZLINK_STREAM_CONNECTOR_WITH_LZ4 OR ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4)
  find_path(ZLINK_LZ4_INCLUDE_DIR lz4.h)
  find_library(ZLINK_LZ4_LIBRARY NAMES lz4 liblz4)
  if(ZLINK_LZ4_INCLUDE_DIR AND ZLINK_LZ4_LIBRARY)
    if(ZLINK_STREAM_CONNECTOR_WITH_LZ4)
      target_include_directories(zlink_stream_connector PRIVATE
        ${ZLINK_LZ4_INCLUDE_DIR})
      target_link_libraries(zlink_stream_connector PRIVATE ${ZLINK_LZ4_LIBRARY})
    endif()
    if(ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4)
      target_include_directories(zlink_framework PRIVATE
        ${ZLINK_LZ4_INCLUDE_DIR})
      target_link_libraries(zlink_framework PRIVATE ${ZLINK_LZ4_LIBRARY})
    endif()
  else()
    FetchContent_Declare(
      zlink_lz4
      URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(zlink_lz4)
    if(ZLINK_STREAM_CONNECTOR_WITH_LZ4)
      target_sources(zlink_stream_connector PRIVATE
        ${zlink_lz4_SOURCE_DIR}/lib/lz4.c)
      target_include_directories(zlink_stream_connector PRIVATE
        ${zlink_lz4_SOURCE_DIR}/lib)
    endif()
    if(ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4)
      target_sources(zlink_framework PRIVATE
        ${zlink_lz4_SOURCE_DIR}/lib/lz4.c)
      target_include_directories(zlink_framework PRIVATE
        ${zlink_lz4_SOURCE_DIR}/lib)
    endif()
  endif()
endif()
target_compile_definitions(zlink_framework PRIVATE
  $<$<BOOL:${ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4}>:ZLINK_FRAMEWORK_STREAM_WITH_LZ4>)
target_compile_definitions(zlink_stream_connector PRIVATE
  $<$<BOOL:${ZLINK_STREAM_CONNECTOR_WITH_LZ4}>:ZLINK_STREAM_CONNECTOR_WITH_LZ4>)

add_library(zlink_stream_connector_codecs INTERFACE)
add_library(zlink::stream_connector_codecs ALIAS zlink_stream_connector_codecs)
set_target_properties(zlink_stream_connector_codecs PROPERTIES
  EXPORT_NAME stream_connector_codecs)
target_compile_features(zlink_stream_connector_codecs INTERFACE cxx_std_20)
target_include_directories(zlink_stream_connector_codecs INTERFACE
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/framework/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zlink_stream_connector_codecs INTERFACE
  zlink::stream_connector
  nlohmann_json::nlohmann_json)

if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)
  add_library(zlink_stream_e2e_client INTERFACE)
  add_library(zlink::stream_e2e_client ALIAS zlink_stream_e2e_client)
  set_target_properties(zlink_stream_e2e_client PROPERTIES
    EXPORT_NAME stream_e2e_client)
  target_compile_features(zlink_stream_e2e_client INTERFACE cxx_std_20)
  target_include_directories(zlink_stream_e2e_client INTERFACE
    $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/e2e-client/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(zlink_stream_e2e_client INTERFACE
    zlink::stream_connector
    zlink::stream_connector_codecs)
endif()

add_library(zlink_stream_connector_throwing INTERFACE)
add_library(zlink::stream_connector_throwing ALIAS zlink_stream_connector_throwing)
set_target_properties(zlink_stream_connector_throwing PROPERTIES
  EXPORT_NAME stream_connector_throwing)
target_compile_features(zlink_stream_connector_throwing INTERFACE cxx_std_20)
target_include_directories(zlink_stream_connector_throwing INTERFACE
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/throwing-adapter/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zlink_stream_connector_throwing INTERFACE
  zlink::stream_connector)

if(ZLINK_STREAM_CONNECTOR_BUILD_UNREAL)
  add_library(zlink_unreal_stream_connector STATIC
    connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp)
  set_target_properties(zlink_unreal_stream_connector PROPERTIES
    EXPORT_NAME unreal_stream_connector)
  target_compile_features(zlink_unreal_stream_connector PUBLIC cxx_std_20)
  target_include_directories(zlink_unreal_stream_connector PUBLIC
    $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/engines/unreal/Source/ZLinkStreamConnector/Public>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ZLinkStreamConnector>)
  target_link_libraries(zlink_unreal_stream_connector PRIVATE
    zlink::stream_connector
    zlink::stream_connector_codecs)
  if(ZLINK_STREAM_CONNECTOR_WITH_LZ4)
    if(ZLINK_LZ4_INCLUDE_DIR AND ZLINK_LZ4_LIBRARY)
      target_include_directories(zlink_unreal_stream_connector PRIVATE
        ${ZLINK_LZ4_INCLUDE_DIR})
      target_link_libraries(zlink_unreal_stream_connector PRIVATE
        ${ZLINK_LZ4_LIBRARY})
    else()
      target_sources(zlink_unreal_stream_connector PRIVATE
        ${zlink_lz4_SOURCE_DIR}/lib/lz4.c)
      target_include_directories(zlink_unreal_stream_connector PRIVATE
        ${zlink_lz4_SOURCE_DIR}/lib)
    endif()
  endif()
  target_compile_definitions(zlink_unreal_stream_connector PRIVATE
    $<$<BOOL:${ZLINK_STREAM_CONNECTOR_WITH_LZ4}>:ZLINK_UNREAL_STREAM_CONNECTOR_WITH_LZ4>)
endif()

if(ZLINK_STREAM_CONNECTOR_BUILD_GODOT)
  add_library(zlink_godot_stream_connector STATIC
    connector/engines/godot/src/zlink_godot_stream_connector.cpp)
  target_compile_features(zlink_godot_stream_connector PUBLIC cxx_std_20)
  target_include_directories(zlink_godot_stream_connector PUBLIC
    $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/engines/godot/include>)
  target_link_libraries(zlink_godot_stream_connector PRIVATE
    zlink::stream_connector
    zlink::stream_connector_codecs)
endif()

if(ZLINK_STREAM_CONNECTOR_BUILD_AXMOL)
  add_library(zlink_axmol_stream_connector STATIC
    connector/engines/axmol/src/zlink_axmol_stream_connector.cpp)
  target_compile_features(zlink_axmol_stream_connector PUBLIC cxx_std_20)
  target_include_directories(zlink_axmol_stream_connector PUBLIC
    $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/engines/axmol/include>)
  target_link_libraries(zlink_axmol_stream_connector PRIVATE
    zlink::stream_connector
    zlink::stream_connector_codecs)
endif()

function(zlink_enable_coverage target_name)
  if(ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
      target_link_options(${target_name} PUBLIC --coverage)
    else()
      message(FATAL_ERROR
        "ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE requires GCC or Clang compatible coverage flags.")
    endif()
  endif()
endfunction()

zlink_enable_coverage(zlink_framework)
zlink_enable_coverage(zlink_http_client)
zlink_enable_coverage(zlink_stream_connector)
if(TARGET zlink_unreal_stream_connector)
  zlink_enable_coverage(zlink_unreal_stream_connector)
endif()
if(TARGET zlink_godot_stream_connector)
  zlink_enable_coverage(zlink_godot_stream_connector)
endif()
if(TARGET zlink_axmol_stream_connector)
  zlink_enable_coverage(zlink_axmol_stream_connector)
endif()

function(add_zlink_framework_codec_extension target_name alias_name include_dir)
  add_library(${target_name} INTERFACE)
  add_library(${alias_name} ALIAS ${target_name})
  string(REPLACE "zlink_framework_codec_" "framework_codec_"
    export_name "${target_name}")
  set_target_properties(${target_name} PROPERTIES EXPORT_NAME ${export_name})
  target_compile_features(${target_name} INTERFACE cxx_std_20)
  target_include_directories(${target_name} INTERFACE
    $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/${include_dir}/include>
    $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(${target_name} INTERFACE zlink::framework)
endfunction()

add_zlink_framework_codec_extension(
  zlink_framework_codec_protobuf
  zlink::framework_codec_protobuf
  extensions/framework-codec-protobuf)
# The protobuf codec carries protobuf wire bytes, so it needs the runtime it
# encodes with. Payload types come from protoc; the extension only serializes them.
find_package(protobuf CONFIG REQUIRED)
target_link_libraries(zlink_framework_codec_protobuf INTERFACE protobuf::libprotobuf)

# Generates protobuf sources for a sample/e2e schema and exposes them as a target.
function(add_zlink_protobuf_schema target_name proto_file)
  get_filename_component(proto_dir "${proto_file}" DIRECTORY)
  add_library(${target_name} STATIC "${proto_file}")
  target_link_libraries(${target_name} PUBLIC protobuf::libprotobuf)
  target_include_directories(${target_name} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
  set_target_properties(${target_name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  protobuf_generate(
    TARGET ${target_name}
    IMPORT_DIRS "${proto_dir}"
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
add_zlink_framework_codec_extension(
  zlink_framework_codec_messagepack
  zlink::framework_codec_messagepack
  extensions/framework-codec-messagepack)

find_package(redis++ CONFIG QUIET)
find_package(libuv CONFIG QUIET)

add_library(zlink_framework_locations_redis INTERFACE)
add_library(zlink::framework_locations_redis ALIAS zlink_framework_locations_redis)
set_target_properties(zlink_framework_locations_redis PROPERTIES
  EXPORT_NAME framework_locations_redis)
target_compile_features(zlink_framework_locations_redis INTERFACE cxx_std_20)
target_include_directories(zlink_framework_locations_redis INTERFACE
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/extensions/framework-locations-redis/include>
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zlink_framework_locations_redis INTERFACE
  zlink::framework_provider_abstractions)
if(TARGET redis++::redis++)
  target_link_libraries(zlink_framework_locations_redis INTERFACE
    redis++::redis++)
  target_compile_definitions(zlink_framework_locations_redis INTERFACE
    ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT=1)
elseif(TARGET redis++::redis++_static)
  target_link_libraries(zlink_framework_locations_redis INTERFACE
    redis++::redis++_static)
  target_compile_definitions(zlink_framework_locations_redis INTERFACE
    ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT=1)
endif()
if(TARGET libuv::uv_a)
  target_link_libraries(zlink_framework_locations_redis INTERFACE
    libuv::uv_a)
elseif(TARGET libuv::uv)
  target_link_libraries(zlink_framework_locations_redis INTERFACE
    libuv::uv)
endif()

set(ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES "")
set(ZLINK_FRAMEWORK_REDIS_EXPORTS_REDIS_PLUS_PLUS OFF)
set(ZLINK_FRAMEWORK_REDIS_EXPORTS_LIBUV OFF)
if(TARGET redis++::redis++ OR TARGET redis++::redis++_static)
  set(ZLINK_FRAMEWORK_REDIS_EXPORTS_REDIS_PLUS_PLUS ON)
  string(APPEND ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES
    "find_dependency(redis++ CONFIG)\n")
endif()
if(TARGET libuv::uv_a OR TARGET libuv::uv)
  set(ZLINK_FRAMEWORK_REDIS_EXPORTS_LIBUV ON)
  string(APPEND ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES
    "find_dependency(libuv CONFIG)\n")
endif()

set(ZLINK_FRAMEWORK_CPP_PACKAGE_DEPENDENCY_PREFIX_PATH
  ${CMAKE_PREFIX_PATH})
foreach(dependency_dir IN ITEMS
    "${redis++_DIR}" "${hiredis_DIR}" "${libuv_DIR}")
  if(dependency_dir)
    list(APPEND ZLINK_FRAMEWORK_CPP_PACKAGE_DEPENDENCY_PREFIX_PATH
      "${dependency_dir}")
  endif()
endforeach()

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(TARGETS
    zlink_framework
    zlink_framework_provider_abstractions
    zlink_framework_codec_protobuf
    zlink_framework_codec_messagepack
    zlink_framework_locations_redis
    EXPORT zlink_framework_cppTargets
    COMPONENT Framework)
  install(TARGETS
    zlink_http_client
    EXPORT zlink_http_client_cppTargets
    COMPONENT HttpClient)
endif()
install(TARGETS
  zlink_stream_connector
  zlink_stream_connector_codecs
  zlink_stream_connector_throwing
  EXPORT zlink_stream_connector_cppTargets
  COMPONENT StreamConnector)
if(TARGET zlink_unreal_stream_connector)
  install(TARGETS
    zlink_unreal_stream_connector
    EXPORT zlink_stream_connector_cppTargets
    COMPONENT StreamConnector)
endif()
if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)
  install(TARGETS
    zlink_stream_e2e_client
    EXPORT zlink_stream_connector_cppTargets
    COMPONENT StreamConnector)
endif()
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(DIRECTORY "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT FrameworkDependency
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
  install(FILES "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/libzlink_cpp.a"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT FrameworkDependency)
  install(DIRECTORY "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/cmake/zlink_cpp"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake
    COMPONENT FrameworkDependency)
  install(DIRECTORY framework/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Framework
    FILES_MATCHING PATTERN "*.hpp")
  install(DIRECTORY http-client/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT HttpClient
    FILES_MATCHING PATTERN "*.hpp")
endif()
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(DIRECTORY extensions/framework-codec-protobuf/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Framework
    FILES_MATCHING PATTERN "*.hpp")
  install(DIRECTORY extensions/framework-codec-messagepack/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Framework
    FILES_MATCHING PATTERN "*.hpp")
  install(DIRECTORY extensions/framework-locations-redis/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Framework
    FILES_MATCHING PATTERN "*.hpp")
endif()
install(DIRECTORY connector/core/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT StreamConnector
  FILES_MATCHING PATTERN "*.hpp")
if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)
  install(DIRECTORY connector/e2e-client/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT StreamConnector
    FILES_MATCHING PATTERN "*.hpp")
endif()
install(DIRECTORY connector/throwing-adapter/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT StreamConnector
  FILES_MATCHING PATTERN "*.hpp")
if(TARGET zlink_unreal_stream_connector)
  install(DIRECTORY connector/engines/unreal/Source/ZLinkStreamConnector/Public/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ZLinkStreamConnector
    COMPONENT StreamConnector
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
endif()
if(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME AND EXISTS "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME}")
  if(WIN32)
    install(FILES "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME}"
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT FrameworkDependency)
  elseif(UNIX AND NOT APPLE)
    get_filename_component(
      ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_DIR
      "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME}"
      DIRECTORY)
    file(GLOB ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_FILES
      "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_DIR}/libzlink.so*")
    install(FILES ${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_FILES}
      DESTINATION ${CMAKE_INSTALL_LIBDIR}
      COMPONENT FrameworkDependency)
  else()
    install(FILES "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME}"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}
      COMPONENT FrameworkDependency)
  endif()
endif()
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(EXPORT zlink_framework_cppTargets
    NAMESPACE zlink::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_framework_cpp
    COMPONENT Framework)
  install(EXPORT zlink_http_client_cppTargets
    NAMESPACE zlink::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_http_client_cpp
    COMPONENT HttpClient)
endif()
install(EXPORT zlink_stream_connector_cppTargets
  NAMESPACE zlink::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_stream_connector_cpp
  COMPONENT StreamConnector)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/zlink_framework_cppConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_framework_cppConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_framework_cpp)
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/zlink_http_client_cppConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_http_client_cppConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_http_client_cpp)
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/zlink_stream_connector_cppConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_stream_connector_cppConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_stream_connector_cpp)
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/zlink_framework_cppConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_framework_cpp
    COMPONENT Framework)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/zlink_http_client_cppConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_http_client_cpp
    COMPONENT HttpClient)
endif()
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/zlink_stream_connector_cppConfig.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_stream_connector_cpp
  COMPONENT StreamConnector)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
endif()

function(zlink_framework_cpp_enable_test_assertions target_name)
  # Several process tests keep syscall, mailbox, and future side effects in
  # assertion expressions. Release evidence must execute those expressions;
  # disabling assertions would silently remove the operation under test.
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target_name} PRIVATE -UNDEBUG)
  endif()
endfunction()

if(ZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS)
  add_executable(test_cpp_framework_raw_route_port_contract
    tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp
    framework/src/runtime/backend/raw_route_port.cpp)
  target_compile_features(test_cpp_framework_raw_route_port_contract PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_raw_route_port_contract PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  target_link_libraries(test_cpp_framework_raw_route_port_contract PRIVATE
    zlink::cpp
    Threads::Threads)
  add_test(NAME test_cpp_framework_raw_route_port_contract
    COMMAND test_cpp_framework_raw_route_port_contract)
  set_tests_properties(test_cpp_framework_raw_route_port_contract PROPERTIES
    LABELS "framework-contract;framework-foundation;raw-binding"
    TIMEOUT 30)

  add_executable(test_cpp_framework_service_wire_codec
    tests/Zlink.Framework.UnitTests/test_cpp_framework_service_wire_codec.cpp
    framework/src/runtime/protocol/service_wire_codec.cpp
    framework/src/runtime/mesh/service_topology_registry.cpp)
  target_compile_features(test_cpp_framework_service_wire_codec PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_service_wire_codec PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_service_wire_codec PRIVATE
    zlink::cpp
    Threads::Threads)
  add_test(NAME test_cpp_framework_service_wire_codec
    COMMAND test_cpp_framework_service_wire_codec)
  set_tests_properties(test_cpp_framework_service_wire_codec PROPERTIES
    LABELS "framework-unit;framework-foundation;protocol"
    TIMEOUT 30)

  add_executable(test_cpp_framework_operation_registry
    tests/Zlink.Framework.UnitTests/test_cpp_framework_operation_registry.cpp
    framework/src/runtime/foundation/operation_registry.cpp)
  target_compile_features(test_cpp_framework_operation_registry PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_operation_registry PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  target_link_libraries(test_cpp_framework_operation_registry PRIVATE Threads::Threads)
  add_test(NAME test_cpp_framework_operation_registry
    COMMAND test_cpp_framework_operation_registry)
  set_tests_properties(test_cpp_framework_operation_registry PROPERTIES
    LABELS "framework-unit;framework-foundation;operation;resource"
    TIMEOUT 30)

  add_executable(test_cpp_framework_m6a_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp)
  target_compile_features(test_cpp_framework_m6a_runtime PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_m6a_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_m6a_runtime PRIVATE
    zlink_framework_m6a_runtime
    Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_m6a_runtime)
  add_test(NAME test_cpp_framework_m6a_runtime
    COMMAND test_cpp_framework_m6a_runtime)
  set_tests_properties(test_cpp_framework_m6a_runtime PROPERTIES
    LABELS "framework-unit;framework-m6-runtime;topology;mailbox;liveness;raw-binding"
    TIMEOUT 30)

  add_executable(test_cpp_framework_m6b_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp)
  target_compile_features(test_cpp_framework_m6b_runtime PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_m6b_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_m6b_runtime PRIVATE
    zlink_framework_m6b_runtime
    zlink_framework
    Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_m6b_runtime)
  add_test(NAME test_cpp_framework_m6b_runtime
    COMMAND test_cpp_framework_m6b_runtime)
  set_tests_properties(test_cpp_framework_m6b_runtime PROPERTIES
    LABELS "framework-unit;framework-m6-runtime;stateful;spot;actor;stream-session"
    TIMEOUT 30)

  add_executable(test_cpp_framework_m6c_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_m6c_runtime.cpp)
  target_compile_features(test_cpp_framework_m6c_runtime PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_m6c_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_m6c_runtime PRIVATE
    zlink_framework
    Threads::Threads)
  add_test(NAME test_cpp_framework_m6c_runtime
    COMMAND test_cpp_framework_m6c_runtime)
  set_tests_properties(test_cpp_framework_m6c_runtime PROPERTIES
    LABELS "framework-unit;framework-m6-runtime;maintenance;relocation;recovery;resource"
    TIMEOUT 30)

  add_executable(test_cpp_framework_host_lifecycle
    tests/Zlink.Framework.UnitTests/test_cpp_framework_host_lifecycle.cpp)
  target_compile_features(test_cpp_framework_host_lifecycle PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_host_lifecycle PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  target_link_libraries(test_cpp_framework_host_lifecycle PRIVATE
    zlink_framework
    Threads::Threads)
  add_test(NAME test_cpp_framework_host_lifecycle
    COMMAND test_cpp_framework_host_lifecycle)
  set_tests_properties(test_cpp_framework_host_lifecycle PROPERTIES
    LABELS "framework-unit;framework-m6-runtime;maintenance;relocation;termination"
    TIMEOUT 30)

endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_TESTS)
  add_executable(zlink_cpp_framework_mesh_node_vertical_test
    tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp)
  zlink_framework_cpp_enable_test_assertions(
    zlink_cpp_framework_mesh_node_vertical_test)
  target_include_directories(zlink_cpp_framework_mesh_node_vertical_test PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  target_link_libraries(zlink_cpp_framework_mesh_node_vertical_test PRIVATE
    zlink::framework
    Threads::Threads)
  add_test(NAME zlink_cpp_framework_mesh_node_vertical_test
    COMMAND zlink_cpp_framework_mesh_node_vertical_test)
  set_tests_properties(zlink_cpp_framework_mesh_node_vertical_test PROPERTIES
    LABELS "framework-unit;framework-zlink;framework-zlink-channel;framework-zlink-spot;framework-zlink-actor-gateway"
    TIMEOUT 30)

  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googletest)
  endif()
  if(TARGET gtest AND NOT TARGET GTest::gtest)
    add_library(GTest::gtest ALIAS gtest)
  endif()
  if(TARGET gmock AND NOT TARGET GTest::gmock)
    add_library(GTest::gmock ALIAS gmock)
  endif()
  if(TARGET gtest_main AND NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main ALIAS gtest_main)
  endif()

  function(target_link_zlink_test_harness target_name)
    target_link_libraries(${target_name} PRIVATE GTest::gtest GTest::gmock)
    zlink_enable_coverage(${target_name})
  endfunction()

  function(add_zlink_framework_test target_name source_file labels)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE zlink::framework)
    target_link_zlink_test_harness(${target_name})
    target_include_directories(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES
      LABELS "${labels}"
      TIMEOUT 30)
  endfunction()

  function(add_zlink_connector_test target_name source_file labels)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE zlink::stream_connector)
    target_link_zlink_test_harness(${target_name})
    target_include_directories(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/src)
    # Test sources include Boost directly; they must resolve the same Boost
    # tree as the connector objects (see zlink_framework_cpp_add_boost_headers).
    zlink_framework_cpp_add_boost_headers(${target_name})
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES
      LABELS "${labels}"
      TIMEOUT 30)
  endfunction()

  function(add_zlink_http_client_test target_name source_file labels)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE zlink::http_client)
    target_link_zlink_test_harness(${target_name})
    target_include_directories(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/http-client/src)
    # Test sources include Boost (beast test server); they must resolve the
    # same Boost tree as the http client objects.
    zlink_framework_cpp_add_boost_headers(${target_name})
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES
      LABELS "${labels}"
      TIMEOUT 30)
  endfunction()

  add_executable(test_cpp_framework_contract_headers
    tests/Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp)
  target_link_libraries(test_cpp_framework_contract_headers PRIVATE
    zlink::framework
    zlink::framework_codec_protobuf
    zlink::http_client
    zlink::stream_connector
    zlink::stream_connector_codecs
    zlink::stream_e2e_client
    zlink::stream_connector_throwing
    GTest::gtest
    GTest::gmock)
  add_test(NAME test_cpp_framework_contract_headers
    COMMAND test_cpp_framework_contract_headers)
  set_tests_properties(test_cpp_framework_contract_headers PROPERTIES
    LABELS "framework-contract"
    TIMEOUT 30)
  add_test(NAME test_cpp_framework_async_contract
    COMMAND test_cpp_framework_contract_headers)
  set_tests_properties(test_cpp_framework_async_contract PROPERTIES
    LABELS "framework-contract;framework-unit;async"
    TIMEOUT 30)

  add_executable(test_cpp_framework_sample_parity
    tests/Zlink.Framework.ContractTests/test_cpp_framework_sample_parity.cpp)
  target_link_libraries(test_cpp_framework_sample_parity PRIVATE
    zlink::framework
    zlink::framework_codec_protobuf
    zlink::stream_connector
    GTest::gtest
    GTest::gmock)
  target_include_directories(test_cpp_framework_sample_parity PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  add_test(NAME test_cpp_framework_sample_parity
    COMMAND test_cpp_framework_sample_parity)
  set_tests_properties(test_cpp_framework_sample_parity PROPERTIES
    LABELS "framework-contract;framework-sample-parity"
    TIMEOUT 30)
  add_test(NAME test_cpp_framework_parity_contract
    COMMAND test_cpp_framework_sample_parity)
  set_tests_properties(test_cpp_framework_parity_contract PROPERTIES
    LABELS "framework-regression;framework-sample-parity;parity"
    TIMEOUT 30)

  add_executable(test_cpp_framework_layout_contract
    tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp)
  target_compile_definitions(test_cpp_framework_layout_contract PRIVATE
    ZLINK_FRAMEWORK_CPP_SOURCE_DIR="${ZLINK_FRAMEWORK_CPP_DIR}")
  target_compile_features(test_cpp_framework_layout_contract PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_layout_contract)
  add_test(NAME test_cpp_framework_layout_contract
    COMMAND test_cpp_framework_layout_contract)
  set_tests_properties(test_cpp_framework_layout_contract PROPERTIES
    LABELS "framework-contract"
    TIMEOUT 30)

  add_executable(test_cpp_framework_target_contract
    tests/Zlink.Framework.ContractTests/test_cpp_framework_target_contract.cpp)
  target_compile_definitions(test_cpp_framework_target_contract PRIVATE
    ZLINK_FRAMEWORK_CPP_SOURCE_DIR="${ZLINK_FRAMEWORK_CPP_DIR}")
  target_compile_features(test_cpp_framework_target_contract PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_target_contract)
  add_test(NAME test_cpp_framework_target_contract
    COMMAND test_cpp_framework_target_contract)
  set_tests_properties(test_cpp_framework_target_contract PROPERTIES
    LABELS "framework-contract"
    TIMEOUT 60)
  add_test(
    NAME test_cpp_framework_common_e2e_inventory
    COMMAND bash
      ${CMAKE_CURRENT_LIST_DIR}/e2e/verify_common_inventory.sh)
  set_tests_properties(test_cpp_framework_common_e2e_inventory PROPERTIES
    LABELS "framework-contract;framework-e2e"
    TIMEOUT 30)
  set(ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS
    -D ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
    -D ZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR})
  if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS
      -D ZLINK_FRAMEWORK_CPP_TOOLING_CMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
  endif()
  foreach(tooling_dependency IN ITEMS
      protobuf_DIR
      absl_DIR
      utf8_range_DIR
      ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX)
    if(DEFINED ${tooling_dependency} AND NOT "${${tooling_dependency}}" STREQUAL "")
      list(APPEND ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS
        -D ZLINK_FRAMEWORK_CPP_TOOLING_${tooling_dependency}=${${tooling_dependency}})
    endif()
  endforeach()
  add_test(
    NAME test_cpp_framework_tooling_contract
    COMMAND ${CMAKE_COMMAND}
      ${ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS}
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.ContractTests/verify_tooling_contract.cmake)
  set_tests_properties(test_cpp_framework_tooling_contract PROPERTIES
    LABELS "framework-contract;framework-package;framework-tooling"
    RESOURCE_LOCK "zlink-cpp-vcpkg"
    TIMEOUT 120)
  add_test(
    NAME test_cpp_framework_label_contract
    COMMAND ${CMAKE_COMMAND}
      -D ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
      -D ZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -D ZLINK_FRAMEWORK_CPP_EXPECT_COVERAGE_LABEL=${ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE}
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.ContractTests/verify_ctest_label_contract.cmake)
  set_tests_properties(test_cpp_framework_label_contract PROPERTIES
    LABELS "framework-contract;framework-regression"
    TIMEOUT 30)

  add_zlink_framework_test(
    test_cpp_framework_app_host
    tests/Zlink.Framework.UnitTests/test_cpp_framework_app_host.cpp
    "framework-unit;framework-host;framework-config;framework-http;framework-http-e2e")
  add_zlink_framework_test(
    test_cpp_framework_client_server_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_client_server_runtime.cpp
    "framework-unit;framework-regression;framework-client-server;framework-monitoring")
  target_include_directories(test_cpp_framework_client_server_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  set_tests_properties(test_cpp_framework_client_server_runtime PROPERTIES
    RESOURCE_LOCK "zlink_cpp_heavy_runtime"
    TIMEOUT 30)
  add_zlink_framework_test(
    test_cpp_framework_actor_gateway
    tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_gateway.cpp
    "framework-unit;framework-regression;framework-actor;stream")
  target_link_libraries(test_cpp_framework_app_host PRIVATE zlink::http_client)
  if(ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE)
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTP_ENDPOINT "http://127.0.0.1:18180")
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_INVALID_ENDPOINT "https://127.0.0.1:18543")
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_ENDPOINT "https://127.0.0.1:18544")
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_CLIENT_BASE_URL "https://localhost:18544")
  else()
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTP_ENDPOINT "http://127.0.0.1:18080")
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_INVALID_ENDPOINT "https://127.0.0.1:18443")
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_ENDPOINT "https://127.0.0.1:18444")
    set(ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_CLIENT_BASE_URL "https://localhost:18444")
  endif()
  target_compile_definitions(test_cpp_framework_app_host PRIVATE
    ZLINK_FRAMEWORK_HTTP_TEST_HTTP_ENDPOINT="${ZLINK_FRAMEWORK_HTTP_TEST_HTTP_ENDPOINT}"
    ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_INVALID_ENDPOINT="${ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_INVALID_ENDPOINT}"
    ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_ENDPOINT="${ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_ENDPOINT}"
    ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_CLIENT_BASE_URL="${ZLINK_FRAMEWORK_HTTP_TEST_HTTPS_CLIENT_BASE_URL}")
  if(ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL)
    target_compile_definitions(test_cpp_framework_app_host PRIVATE
      ZLINK_FRAMEWORK_HTTP_TEST_WITH_OPENSSL
      ZLINK_FRAMEWORK_HTTP_TEST_CERT="${ZLINK_HTTP_CLIENT_TEST_CERT}"
      ZLINK_FRAMEWORK_HTTP_TEST_KEY="${ZLINK_HTTP_CLIENT_TEST_KEY}")
  endif()
  add_test(NAME test_cpp_framework_http_integration
    COMMAND test_cpp_framework_app_host)
  set_tests_properties(test_cpp_framework_http_integration PROPERTIES
    LABELS "framework-integration;framework-http;framework-http-e2e;http"
    TIMEOUT 30)
  if(ZLINK_FRAMEWORK_CPP_REQUIRE_HTTP_PERF_REPORT)
    add_test(NAME test_cpp_framework_http_perf_gate
      COMMAND ${CMAKE_COMMAND}
        -D ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.PerfTests/http_perf_gate.cmake)
    set_tests_properties(test_cpp_framework_http_perf_gate PROPERTIES
      ENVIRONMENT "ZLINK_FRAMEWORK_HTTP_PERF_REQUIRED=1"
      LABELS "framework-http-perf;framework-regression;http"
      TIMEOUT 30)
  endif()
  add_executable(test_cpp_framework_entry_spot_admission_burst
    tests/Zlink.Framework.PerfTests/entry_spot_admission_burst.cpp)
  target_include_directories(test_cpp_framework_entry_spot_admission_burst PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_entry_spot_admission_burst PRIVATE
    zlink_framework Threads::Threads)
  add_test(NAME test_cpp_framework_entry_spot_admission_burst
    COMMAND test_cpp_framework_entry_spot_admission_burst)
  set_tests_properties(test_cpp_framework_entry_spot_admission_burst PROPERTIES
    LABELS "framework-perf-smoke;framework-regression;spot-actor"
    TIMEOUT 30)
  set_tests_properties(
    test_cpp_framework_app_host
    test_cpp_framework_http_integration
    PROPERTIES
      RESOURCE_LOCK zlink_cpp_framework_http_ports)
  add_zlink_framework_test(
    test_cpp_framework_DI_scope
    tests/Zlink.Framework.UnitTests/test_cpp_framework_di.cpp
    "framework-unit;framework-regression;DI;scope")
  add_zlink_framework_test(
    test_cpp_framework_runtime_integration
    tests/Zlink.Framework.UnitTests/test_cpp_framework_runtime_integration.cpp
    "framework-unit;framework-integration;runtime")
  add_zlink_framework_test(
    test_cpp_framework_async_task
    tests/Zlink.Framework.UnitTests/test_cpp_framework_async_task.cpp
    "framework-unit;framework-regression;async")
  add_zlink_framework_test(
    test_cpp_framework_execution
    tests/Zlink.Framework.UnitTests/test_cpp_framework_execution.cpp
    "framework-unit;framework-regression;execution;spot;yield")
  target_include_directories(test_cpp_framework_execution PRIVATE
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_serializer_registry
    tests/Zlink.Framework.UnitTests/test_cpp_framework_serializer_registry.cpp
    "framework-unit;serializer")
  add_executable(test_cpp_framework_location_key_codec
    tests/Zlink.Framework.UnitTests/test_cpp_framework_location_key_codec.cpp)
  target_compile_features(test_cpp_framework_location_key_codec PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_location_key_codec)
  target_link_libraries(test_cpp_framework_location_key_codec PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_location_key_codec PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  add_test(NAME test_cpp_framework_location_key_codec
    COMMAND test_cpp_framework_location_key_codec)
  set_tests_properties(test_cpp_framework_location_key_codec PROPERTIES
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)
  add_executable(test_cpp_framework_in_memory_location_store
    tests/Zlink.Framework.UnitTests/test_cpp_framework_in_memory_location_store.cpp)
  target_compile_features(test_cpp_framework_in_memory_location_store PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_in_memory_location_store)
  target_link_libraries(test_cpp_framework_in_memory_location_store PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_in_memory_location_store PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  add_test(NAME test_cpp_framework_in_memory_location_store
    COMMAND test_cpp_framework_in_memory_location_store)
  set_tests_properties(test_cpp_framework_in_memory_location_store PROPERTIES
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)

  add_executable(test_cpp_framework_opaque_store_providers
    tests/Zlink.Framework.UnitTests/test_cpp_framework_opaque_store_providers.cpp)
  target_compile_features(test_cpp_framework_opaque_store_providers PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_opaque_store_providers)
  target_link_libraries(test_cpp_framework_opaque_store_providers PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_opaque_store_providers PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  add_test(NAME test_cpp_framework_opaque_store_providers
    COMMAND test_cpp_framework_opaque_store_providers)
  set_tests_properties(test_cpp_framework_opaque_store_providers PROPERTIES
    LABELS "framework-unit;framework-location;framework-regression"
    TIMEOUT 30)
  add_executable(test_cpp_framework_location_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_location_runtime.cpp)
  target_compile_features(test_cpp_framework_location_runtime PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_location_runtime)
  target_link_libraries(test_cpp_framework_location_runtime PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_location_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  add_test(NAME test_cpp_framework_location_runtime
    COMMAND test_cpp_framework_location_runtime)
  set_tests_properties(test_cpp_framework_location_runtime PROPERTIES
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)
  add_executable(test_cpp_framework_location_lifecycle
    tests/Zlink.Framework.UnitTests/test_cpp_framework_location_lifecycle.cpp)
  target_compile_features(test_cpp_framework_location_lifecycle PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_location_lifecycle)
  target_link_libraries(test_cpp_framework_location_lifecycle PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_location_lifecycle PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  add_test(NAME test_cpp_framework_location_lifecycle
    COMMAND test_cpp_framework_location_lifecycle)
  set_tests_properties(test_cpp_framework_location_lifecycle PROPERTIES
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)
  add_executable(test_cpp_framework_store_location_resolvers
    tests/Zlink.Framework.UnitTests/test_cpp_framework_store_location_resolvers.cpp)
  target_compile_features(test_cpp_framework_store_location_resolvers PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_store_location_resolvers)
  target_link_libraries(test_cpp_framework_store_location_resolvers PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_store_location_resolvers PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  zlink_framework_cpp_enable_test_assertions(
    test_cpp_framework_store_location_resolvers)
  zlink_framework_cpp_add_boost_headers(test_cpp_framework_store_location_resolvers)
  add_test(NAME test_cpp_framework_store_location_resolvers
    COMMAND test_cpp_framework_store_location_resolvers)
  set_tests_properties(test_cpp_framework_store_location_resolvers PROPERTIES
    LABELS "framework-unit;framework-regression;framework-location"
    RESOURCE_LOCK zlink_cpp_heavy_runtime
    TIMEOUT 30)
  add_zlink_framework_test(
    test_cpp_framework_handler_registry
    tests/Zlink.Framework.UnitTests/test_cpp_framework_handler_registry.cpp
    "framework-unit;handler")
  add_zlink_framework_test(
    test_cpp_framework_channel_messaging
    tests/Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp
    "framework-unit;framework-regression;channel")
  add_zlink_framework_test(
    test_cpp_framework_instance_spot_activation
    tests/Zlink.Framework.UnitTests/test_cpp_framework_instance_spot_activation.cpp
    "framework-unit;framework-regression;spot;instance-activation")
  target_include_directories(test_cpp_framework_instance_spot_activation PRIVATE
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_instance_spot_activation PRIVATE
    GTest::gtest_main)
  # The channel messaging test reaches the service wire codec headers, which include the
  # schema-generated constants header (see test_cpp_framework_service_wire_codec above).
  target_include_directories(test_cpp_framework_channel_messaging PRIVATE
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_backpressure_reliability
    tests/Zlink.Framework.UnitTests/test_cpp_framework_backpressure_reliability.cpp
    "framework-unit;framework-regression;backpressure;reliability")
  add_zlink_framework_test(
    test_cpp_framework_stream_framework
    tests/Zlink.Framework.UnitTests/test_cpp_framework_stream_framework.cpp
    "framework-integration;framework-regression;framework-zlink;framework-zlink-stream;stream")
  target_link_libraries(test_cpp_framework_stream_framework PRIVATE
    zlink::stream_connector)
  zlink_framework_cpp_add_boost_headers(
    test_cpp_framework_stream_framework)
  target_include_directories(test_cpp_framework_stream_framework PRIVATE
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  if(OpenSSL_FOUND AND ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL)
    target_compile_definitions(test_cpp_framework_stream_framework PRIVATE
      ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
      ZLINK_FRAMEWORK_STREAM_TEST_CERT="${ZLINK_HTTP_CLIENT_TEST_CERT}"
      ZLINK_FRAMEWORK_STREAM_TEST_KEY="${ZLINK_HTTP_CLIENT_TEST_KEY}")
    target_link_libraries(test_cpp_framework_stream_framework PRIVATE
      OpenSSL::SSL OpenSSL::Crypto)
  endif()
  add_zlink_framework_test(
    test_cpp_framework_message_flow
    tests/Zlink.Framework.UnitTests/test_cpp_framework_message_flow.cpp
    "framework-unit;framework-regression;framework-observability;diagnostics")
  add_zlink_framework_test(
    test_cpp_framework_monitoring
    tests/Zlink.Framework.UnitTests/test_cpp_framework_monitoring.cpp
    "framework-unit;framework-regression;framework-observability;metrics")
  add_zlink_framework_test(
    test_cpp_framework_messaging
    tests/Zlink.Framework.UnitTests/test_cpp_framework_messaging.cpp
    "framework-unit;framework-regression;messaging")
  target_include_directories(test_cpp_framework_messaging PRIVATE
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)
  add_executable(test_cpp_framework_submit_admission
    tests/Zlink.Framework.UnitTests/test_cpp_framework_submit_admission.cpp
    framework/src/runtime/messaging/async_submit_runtime.cpp
    framework/src/runtime/dispatch/offload_executor.cpp)
  target_link_libraries(test_cpp_framework_submit_admission PRIVATE
    zlink::cpp
    Threads::Threads)
  target_include_directories(test_cpp_framework_submit_admission PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  add_test(NAME test_cpp_framework_submit_admission
    COMMAND test_cpp_framework_submit_admission)
  set_tests_properties(test_cpp_framework_submit_admission PROPERTIES
    LABELS "framework-unit;framework-regression;messaging;submit-admission"
    TIMEOUT 30)
  add_executable(test_cpp_framework_gtest_harness
    tests/Zlink.Framework.UnitTests/test_cpp_framework_gtest_harness.cpp)
  target_link_libraries(test_cpp_framework_gtest_harness PRIVATE
    zlink::framework
    GTest::gtest_main
    GTest::gmock)
  add_test(NAME test_cpp_framework_gtest_harness
    COMMAND test_cpp_framework_gtest_harness)
  set_tests_properties(test_cpp_framework_gtest_harness PROPERTIES
    LABELS "framework-unit;framework-regression;gtest"
    TIMEOUT 30)
  if(TARGET redis++::redis++ OR TARGET redis++::redis++_static)
    add_executable(test_cpp_framework_locations_redis
      tests/Zlink.Framework.UnitTests/test_cpp_framework_locations_redis.cpp)
    zlink_enable_coverage(test_cpp_framework_locations_redis)
    target_link_libraries(test_cpp_framework_locations_redis PRIVATE
      zlink::framework_locations_redis
      GTest::gtest_main)
    add_test(NAME test_cpp_framework_locations_redis
      COMMAND test_cpp_framework_locations_redis)
    set_tests_properties(test_cpp_framework_locations_redis PROPERTIES
      LABELS "framework-extension;framework-location;redis"
      TIMEOUT 30)
  endif()
  add_test(NAME test_cpp_framework_install_consumer
    COMMAND
      ${CMAKE_COMMAND}
      -DZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DZLINK_FRAMEWORK_CPP_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-consumer-install
      "-DZLINK_FRAMEWORK_CPP_DEPENDENCY_PREFIX_PATH=${ZLINK_FRAMEWORK_CPP_PACKAGE_DEPENDENCY_PREFIX_PATH}"
      -DZLINK_FRAMEWORK_CPP_EXPECT_REDIS_PLUS_PLUS=${ZLINK_FRAMEWORK_REDIS_EXPORTS_REDIS_PLUS_PLUS}
      -DZLINK_FRAMEWORK_CPP_EXPECT_LIBUV=${ZLINK_FRAMEWORK_REDIS_EXPORTS_LIBUV}
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.PackageTests/install_consumer.cmake)
  set_tests_properties(test_cpp_framework_install_consumer PROPERTIES
    LABELS "framework-package;framework-regression;connector-package"
    TIMEOUT 120)
  add_zlink_connector_test(
    test_cpp_stream_connector
    tests/Systems.Zlink.Stream.Connector.Tests/test_cpp_stream_connector.cpp
    "connector-unit;connector-integration;connector-e2e;connector-contract;connector-protocol;connector-transport;connector-typed;connector-coroutine;connector-dispatch;connector-timeout")
  # These tests each create a high-thread-count local runtime.  Serialise the
  # pair so CTest does not exercise their process-level resource usage as if it
  # were a product concurrency contract.
  set_tests_properties(test_cpp_stream_connector PROPERTIES
    RESOURCE_LOCK zlink_cpp_heavy_runtime)
  target_link_libraries(test_cpp_stream_connector PRIVATE
    zlink::stream_connector_codecs
    zlink::stream_e2e_client)
  if(ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL)
    target_compile_definitions(test_cpp_stream_connector PRIVATE
      ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
      ZLINK_STREAM_CONNECTOR_TEST_CERT="${ZLINK_HTTP_CLIENT_TEST_CERT}"
      ZLINK_STREAM_CONNECTOR_TEST_KEY="${ZLINK_HTTP_CLIENT_TEST_KEY}")
    target_link_libraries(test_cpp_stream_connector PRIVATE
      OpenSSL::SSL
      OpenSSL::Crypto)
  endif()
  add_executable(sample_stream_connector_core_client
    connector/core/samples/core_client_smoke.cpp)
  target_link_libraries(sample_stream_connector_core_client PRIVATE
    zlink::stream_connector)
  add_test(NAME sample_stream_connector_core_client
    COMMAND sample_stream_connector_core_client)
  set_tests_properties(sample_stream_connector_core_client PROPERTIES
    LABELS "connector-e2e;connector-contract"
    RESOURCE_LOCK zlink_cpp_heavy_runtime
    TIMEOUT 30)

  add_executable(sample_stream_e2e_client
    connector/e2e-client/samples/e2e_client_smoke.cpp)
  target_link_libraries(sample_stream_e2e_client PRIVATE
    zlink::stream_e2e_client)
  add_test(NAME sample_stream_e2e_client
    COMMAND sample_stream_e2e_client)
  set_tests_properties(sample_stream_e2e_client PROPERTIES
    LABELS "connector-e2e"
    RESOURCE_LOCK zlink_cpp_heavy_runtime
    TIMEOUT 30)

  add_executable(connector_perf_client
    connector/perf/connector_perf_client.cpp)
  target_include_directories(connector_perf_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/src)
  target_link_libraries(connector_perf_client PRIVATE
    zlink::stream_e2e_client)
  add_custom_target(connector_perf_smoke
    DEPENDS connector_perf_client)
  add_test(NAME connector_perf_smoke
    COMMAND connector_perf_client
      --clients 100
      --workers 2
      --duration 5s
      --warmup 0s
      --request-timeout-ms 100
      --loopback
      --report ${CMAKE_CURRENT_BINARY_DIR}/connector-perf-smoke.json)
  set_tests_properties(connector_perf_smoke PROPERTIES
    LABELS "connector-perf-smoke;connector-perf-scale"
    TIMEOUT 30)

  add_zlink_http_client_test(
    test_cpp_http_client
    http-client/tests/test_cpp_http_client.cpp
    "http-client-contract;http-client-unit;http-client-e2e;http-client-regression")
  if(ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL)
    target_compile_definitions(test_cpp_http_client PRIVATE
      ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
      ZLINK_HTTP_CLIENT_TEST_CERT="${ZLINK_HTTP_CLIENT_TEST_CERT}"
      ZLINK_HTTP_CLIENT_TEST_KEY="${ZLINK_HTTP_CLIENT_TEST_KEY}")
    target_link_libraries(test_cpp_http_client PRIVATE
      OpenSSL::SSL
      OpenSSL::Crypto)
    set_tests_properties(test_cpp_http_client PROPERTIES
      LABELS "http-client-contract;http-client-unit;http-client-e2e;http-client-https;http-client-regression")
  endif()

  add_executable(test_unreal_stream_connector
    tests/Zlink.Unreal.Stream.Connector.Tests/test_unreal_stream_connector.cpp)
  target_link_libraries(test_unreal_stream_connector PRIVATE
    zlink_unreal_stream_connector
    GTest::gtest
    GTest::gmock)
  add_test(NAME test_unreal_stream_connector COMMAND test_unreal_stream_connector)
  set_tests_properties(test_unreal_stream_connector PROPERTIES
    LABELS "connector-unreal-contract;connector-unreal-compile;connector-unreal-smoke"
    RESOURCE_LOCK zlink_cpp_heavy_runtime
    TIMEOUT 30)
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_E2E)
  add_executable(zlink_cpp_e2e_store_failure_provider
    e2e/DiscoveryRegistryHa/Server/Provider/main.cpp)
  target_link_libraries(zlink_cpp_e2e_store_failure_provider PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_store_failure_provider PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_store_failure_consumer
    e2e/DiscoveryRegistryHa/Server/Consumer/main.cpp)
  target_link_libraries(zlink_cpp_e2e_store_failure_consumer PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_store_failure_consumer PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_store_failure_client
    e2e/DiscoveryRegistryHa/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_store_failure_client PRIVATE
    zlink::http_client)
  target_include_directories(zlink_cpp_e2e_store_failure_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registry_messaging_provider
    e2e/RegistryMessaging/Server/Provider/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registry_messaging_provider PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_registry_messaging_provider PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registry_messaging_workflow
    e2e/RegistryMessaging/Server/Workflow/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registry_messaging_workflow PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_registry_messaging_workflow PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registry_messaging_consumer
    e2e/RegistryMessaging/Server/Consumer/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registry_messaging_consumer PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_registry_messaging_consumer PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registry_messaging_object_client
    e2e/RegistryMessaging/Server/ObjectClient/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registry_messaging_object_client PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_registry_messaging_object_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registry_messaging_client
    e2e/RegistryMessaging/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registry_messaging_client PRIVATE
    zlink::framework
    zlink::http_client
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_registry_messaging_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_spot_service_play
    e2e/SpotService/Server/Play/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_service_play PRIVATE
    zlink::framework_locations_redis
    zlink::http_client)

  add_executable(zlink_cpp_e2e_spot_service_session
    e2e/SpotService/Server/Session/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_service_session PRIVATE
    zlink::framework_locations_redis
    zlink::http_client)

  add_executable(zlink_cpp_e2e_spot_service_gateway
    e2e/SpotService/Server/Gateway/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_service_gateway PRIVATE
    zlink::framework_locations_redis)

  add_executable(zlink_cpp_e2e_spot_service_multinode
    e2e/SpotService/Server/MultiNode/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_service_multinode PRIVATE
    zlink::framework_locations_redis
    zlink::http_client)

  add_executable(zlink_cpp_e2e_spot_service_multinode_requester
    e2e/SpotService/Server/MultiNodeRequester/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_service_multinode_requester PRIVATE
    zlink::framework_locations_redis)

  add_executable(zlink_cpp_e2e_spot_service_client
    e2e/SpotService/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_service_client PRIVATE
    zlink::http_client
    zlink::stream_e2e_client)

  add_executable(zlink_cpp_e2e_automatic_turn_dispatch_delay
    e2e/AutomaticTurnDispatch/Server/Delay/main.cpp)
  target_link_libraries(zlink_cpp_e2e_automatic_turn_dispatch_delay PRIVATE
    zlink::framework)
  target_include_directories(zlink_cpp_e2e_automatic_turn_dispatch_delay PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_automatic_turn_dispatch_external_api
    e2e/AutomaticTurnDispatch/Server/ExternalApi/main.cpp)
  target_link_libraries(zlink_cpp_e2e_automatic_turn_dispatch_external_api PRIVATE
    zlink::framework)
  zlink_framework_cpp_add_boost_headers(
    zlink_cpp_e2e_automatic_turn_dispatch_external_api)

  add_executable(zlink_cpp_e2e_automatic_turn_dispatch_play
    e2e/AutomaticTurnDispatch/Server/Play/main.cpp)
  target_link_libraries(zlink_cpp_e2e_automatic_turn_dispatch_play PRIVATE
    zlink::framework_locations_redis
    zlink::http_client)
  target_include_directories(zlink_cpp_e2e_automatic_turn_dispatch_play PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_automatic_turn_dispatch_session
    e2e/AutomaticTurnDispatch/Server/Session/main.cpp)
  target_link_libraries(zlink_cpp_e2e_automatic_turn_dispatch_session PRIVATE
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_automatic_turn_dispatch_session PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_automatic_turn_dispatch_client
    e2e/AutomaticTurnDispatch/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_automatic_turn_dispatch_client PRIVATE
    zlink::framework
    zlink::stream_e2e_client)
  target_include_directories(zlink_cpp_e2e_automatic_turn_dispatch_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_library(zlink_cpp_e2e_observability_ops_host STATIC
    e2e/ObservabilityOps/Server/main.cpp)
  target_link_libraries(zlink_cpp_e2e_observability_ops_host PRIVATE
    zlink::framework
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_observability_ops_host PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_observability_ops_session
    e2e/ObservabilityOps/Server/Session/main.cpp)
  add_executable(zlink_cpp_e2e_observability_ops_play
    e2e/ObservabilityOps/Server/Play/main.cpp)
  add_executable(zlink_cpp_e2e_observability_ops_order_workflow
    e2e/ObservabilityOps/Server/OrderWorkflow/main.cpp)
  foreach(observability_target IN ITEMS
      zlink_cpp_e2e_observability_ops_session
      zlink_cpp_e2e_observability_ops_play
      zlink_cpp_e2e_observability_ops_order_workflow)
    target_link_libraries(${observability_target} PRIVATE
      zlink_cpp_e2e_observability_ops_host
      zlink::framework_locations_redis)
    target_include_directories(${observability_target} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  endforeach()

  add_library(zlink_cpp_e2e_observability_ops_client_runner STATIC
    e2e/ObservabilityOps/Trigger/main.cpp)
  target_link_libraries(zlink_cpp_e2e_observability_ops_client_runner PRIVATE
    zlink::framework
    zlink::stream_e2e_client)
  target_include_directories(zlink_cpp_e2e_observability_ops_client_runner PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_observability_ops_client
    e2e/ObservabilityOps/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_observability_ops_client PRIVATE
    zlink_cpp_e2e_observability_ops_client_runner
    zlink::framework
    zlink::stream_e2e_client)
  target_include_directories(zlink_cpp_e2e_observability_ops_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_pubsub_publisher
    e2e/PubSub/Server/Publisher/main.cpp)
  target_link_libraries(zlink_cpp_e2e_pubsub_publisher PRIVATE
    zlink::framework
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_pubsub_publisher PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_pubsub_subscriber
    e2e/PubSub/Server/Subscriber/main.cpp)
  target_link_libraries(zlink_cpp_e2e_pubsub_subscriber PRIVATE
    zlink::framework
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_pubsub_subscriber PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_pubsub_client
    e2e/PubSub/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_pubsub_client PRIVATE
    zlink::framework)
  target_include_directories(zlink_cpp_e2e_pubsub_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_zlink_protobuf_schema(zlink_cpp_e2e_registration_codec_proto
    ${ZLINK_FRAMEWORK_CPP_DIR}/e2e/RegistrationCodec/Shared/registration_codec.proto)
  add_executable(zlink_cpp_e2e_registration_codec_server
    e2e/RegistrationCodec/Server/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registration_codec_server PRIVATE
    zlink::framework
    zlink_cpp_e2e_registration_codec_proto
    zlink::framework_codec_protobuf
    zlink::framework_codec_messagepack)
  target_include_directories(zlink_cpp_e2e_registration_codec_server PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registration_codec_invalid_duplicate
    e2e/RegistrationCodec/Server/InvalidDuplicate/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registration_codec_invalid_duplicate PRIVATE
    zlink::framework
    zlink_cpp_e2e_registration_codec_proto
    zlink::framework_codec_protobuf
    zlink::framework_codec_messagepack)
  target_include_directories(zlink_cpp_e2e_registration_codec_invalid_duplicate PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registration_codec_json_only_peer
    e2e/RegistrationCodec/Server/JsonOnlyPeer/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registration_codec_json_only_peer PRIVATE
    zlink::framework
    zlink_cpp_e2e_registration_codec_proto
    zlink::framework_codec_protobuf
    zlink::framework_codec_messagepack)
  target_include_directories(zlink_cpp_e2e_registration_codec_json_only_peer PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registration_codec_codec_requester
    e2e/RegistrationCodec/Server/CodecRequester/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registration_codec_codec_requester PRIVATE
    zlink::framework
    zlink_cpp_e2e_registration_codec_proto
    zlink::framework_codec_protobuf
    zlink::framework_codec_messagepack)
  target_include_directories(zlink_cpp_e2e_registration_codec_codec_requester PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_registration_codec_client
    e2e/RegistrationCodec/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_registration_codec_client PRIVATE
    zlink::http_client)
  target_include_directories(zlink_cpp_e2e_registration_codec_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_library(zlink_cpp_e2e_spot_actor_transfer_host STATIC
    e2e/SpotActorTransfer/Server/ActorNode/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_actor_transfer_host PRIVATE
    zlink::framework
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_spot_actor_transfer_host PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_spot_actor_transfer_actor_node
    e2e/SpotActorTransfer/Server/ActorNode/entrypoint.cpp)
  add_executable(zlink_cpp_e2e_spot_actor_transfer_session
    e2e/SpotActorTransfer/Server/Session/main.cpp)
  foreach(transfer_target IN ITEMS
      zlink_cpp_e2e_spot_actor_transfer_actor_node
      zlink_cpp_e2e_spot_actor_transfer_session)
    target_link_libraries(${transfer_target} PRIVATE
      zlink_cpp_e2e_spot_actor_transfer_host
      zlink::framework
      zlink::framework_locations_redis)
    target_include_directories(${transfer_target} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  endforeach()

  add_executable(zlink_cpp_e2e_spot_actor_transfer_client
    e2e/SpotActorTransfer/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_spot_actor_transfer_client PRIVATE
    zlink::http_client
    zlink::stream_connector)

  add_executable(zlink_cpp_e2e_to_actor_messaging_actor
    e2e/ToActorMessaging/Server/Actor/main.cpp)
  target_link_libraries(zlink_cpp_e2e_to_actor_messaging_actor PRIVATE
    zlink::framework
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_to_actor_messaging_actor PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_to_actor_messaging_caller
    e2e/ToActorMessaging/Server/Caller/main.cpp)
  target_link_libraries(zlink_cpp_e2e_to_actor_messaging_caller PRIVATE
    zlink::framework
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_to_actor_messaging_caller PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_to_actor_messaging_session
    e2e/ToActorMessaging/Server/Session/main.cpp)
  target_link_libraries(zlink_cpp_e2e_to_actor_messaging_session PRIVATE
    zlink::framework
    zlink::framework_locations_redis)
  target_include_directories(zlink_cpp_e2e_to_actor_messaging_session PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_to_actor_messaging_client
    e2e/ToActorMessaging/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_to_actor_messaging_client PRIVATE
    zlink::http_client
    zlink::stream_connector)
  target_include_directories(zlink_cpp_e2e_to_actor_messaging_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_resilience_lifecycle_provider
    e2e/ResilienceLifecycle/Server/Provider/main.cpp)
  target_link_libraries(zlink_cpp_e2e_resilience_lifecycle_provider PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_resilience_lifecycle_provider PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_resilience_lifecycle_consumer
    e2e/ResilienceLifecycle/Server/Consumer/main.cpp)
  target_link_libraries(zlink_cpp_e2e_resilience_lifecycle_consumer PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_resilience_lifecycle_consumer PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_resilience_lifecycle_client
    e2e/ResilienceLifecycle/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_resilience_lifecycle_client PRIVATE
    zlink::http_client
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_resilience_lifecycle_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_runtime_monitoring_client
    e2e/RuntimeMonitoring/Client/main.cpp)
  target_link_libraries(zlink_cpp_e2e_runtime_monitoring_client PRIVATE
    zlink::http_client)

  add_executable(zlink_cpp_e2e_runtime_monitoring_service
    e2e/RuntimeMonitoring/Server/Service/main.cpp)
  target_link_libraries(zlink_cpp_e2e_runtime_monitoring_service PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_runtime_monitoring_service PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_runtime_monitoring_filtered_service
    e2e/RuntimeMonitoring/Server/FilteredService/main.cpp)
  target_link_libraries(zlink_cpp_e2e_runtime_monitoring_filtered_service PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_runtime_monitoring_filtered_service PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_runtime_monitoring_throwing_service
    e2e/RuntimeMonitoring/Server/ThrowingService/main.cpp)
  target_link_libraries(zlink_cpp_e2e_runtime_monitoring_throwing_service PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_runtime_monitoring_throwing_service PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  add_executable(zlink_cpp_e2e_runtime_monitoring_trigger
    e2e/RuntimeMonitoring/Server/Trigger/main.cpp)
  target_link_libraries(zlink_cpp_e2e_runtime_monitoring_trigger PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    OpenSSL::Crypto)
  target_include_directories(zlink_cpp_e2e_runtime_monitoring_trigger PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  # Trigger support headers include Boost directly; keep it on the same tree
  # as the framework objects.
  zlink_framework_cpp_add_boost_headers(zlink_cpp_e2e_runtime_monitoring_trigger)

  add_executable(zlink_cpp_e2e_submit_admission_role
    e2e/SubmitAdmission/Server/main.cpp)
  target_link_libraries(zlink_cpp_e2e_submit_admission_role PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    zlink::stream_connector)
  target_include_directories(zlink_cpp_e2e_submit_admission_role PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)

  # Cross-language host (G6): the same role modes the peer languages' test
  # hosts expose, so every producer/consumer direction runs against the other
  # language's real package.
  add_executable(zlink_cpp_cross_language_host
    cross-language/cross_language_host.cpp)
  target_link_libraries(zlink_cpp_cross_language_host PRIVATE
    zlink::framework
    zlink::stream_e2e_client)
  target_include_directories(zlink_cpp_cross_language_host PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp)

  # These executables still exercise the removed SpotMesh/SpotNode host surface.
  # Keep them available as explicit migration targets, but do not make a normal
  # framework build compile the retired topology.
  set_target_properties(
    zlink_cpp_e2e_spot_service_play
    zlink_cpp_e2e_spot_service_session
    zlink_cpp_e2e_spot_service_gateway
    zlink_cpp_e2e_spot_service_multinode
    zlink_cpp_e2e_spot_service_multinode_requester
    zlink_cpp_e2e_automatic_turn_dispatch_play
    zlink_cpp_e2e_automatic_turn_dispatch_session
    zlink_cpp_e2e_observability_ops_host
    zlink_cpp_e2e_observability_ops_session
    zlink_cpp_e2e_observability_ops_play
    zlink_cpp_e2e_observability_ops_order_workflow
    zlink_cpp_e2e_spot_actor_transfer_host
    zlink_cpp_e2e_spot_actor_transfer_actor_node
    zlink_cpp_e2e_spot_actor_transfer_session
    zlink_cpp_e2e_to_actor_messaging_actor
    zlink_cpp_e2e_to_actor_messaging_caller
    zlink_cpp_e2e_to_actor_messaging_session
    zlink_cpp_e2e_runtime_monitoring_service
    zlink_cpp_e2e_runtime_monitoring_filtered_service
    zlink_cpp_e2e_runtime_monitoring_throwing_service
    PROPERTIES EXCLUDE_FROM_ALL TRUE)

endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES)
  function(add_zlink_framework_sample target_name source_file label)
    add_executable(${target_name} EXCLUDE_FROM_ALL ${source_file})
    target_link_libraries(${target_name} PRIVATE ${ARGN})
    target_include_directories(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
    # Sample sources include Boost directly (asio-based redis helpers). Without
    # this the vcpkg Boost that rides in on redis++'s interface include path
    # wins for the sample TU while the framework objects use the vendored tree,
    # mixing two Boost versions in one binary (ODR/ABI mismatch, observed as a
    # deterministic SEGV in asio scheduler internals).
    zlink_framework_cpp_add_boost_headers(${target_name})
  endfunction()

  add_zlink_protobuf_schema(sample_cpp_framework_bingo_proto
    ${ZLINK_FRAMEWORK_CPP_DIR}/samples/Bingo/Shared/Contracts/bingo_messages.proto)
  add_zlink_framework_sample(
    sample_cpp_framework_bingo_api
    samples/Bingo/Server/Api/main.cpp
    "framework-sample-bingo;framework-sample-api"
    zlink::framework_locations_redis
    zlink::framework
    zlink::framework_codec_protobuf
    sample_cpp_framework_bingo_proto)
  add_zlink_framework_sample(
    sample_cpp_framework_bingo_matchmaking
    samples/Bingo/Server/Matchmaking/main.cpp
    "framework-sample-bingo;framework-sample-matchmaking"
    zlink::framework_locations_redis
    zlink::framework
    zlink::framework_codec_protobuf
    sample_cpp_framework_bingo_proto)
  add_zlink_framework_sample(
    sample_cpp_framework_bingo_play
    samples/Bingo/Server/Play/main.cpp
    "framework-sample-bingo;framework-sample-play"
    zlink::framework_locations_redis
    zlink::framework
    zlink::framework_codec_protobuf
    sample_cpp_framework_bingo_proto)
  add_zlink_framework_sample(
    sample_cpp_framework_bingo_session
    samples/Bingo/Server/Session/main.cpp
    "framework-sample-bingo;framework-sample-session"
    zlink::framework_locations_redis
    zlink::framework
    zlink::framework_codec_protobuf
    sample_cpp_framework_bingo_proto
    zlink::stream_connector)
  add_executable(sample_cpp_framework_bingo_client
    samples/Bingo/Client/main.cpp)
  target_link_libraries(sample_cpp_framework_bingo_client PRIVATE
    zlink::stream_e2e_client
    zlink::framework_codec_protobuf
    sample_cpp_framework_bingo_proto)
  add_zlink_framework_sample(
    sample_cpp_framework_tictactoe_api
    samples/TicTacToe/Server/Api/main.cpp
    "framework-sample-tictactoe;framework-sample-api"
    zlink::framework_locations_redis
    zlink::framework
    zlink::stream_connector)
  add_zlink_framework_sample(
    sample_cpp_framework_tictactoe_play
    samples/TicTacToe/Server/Play/main.cpp
    "framework-sample-tictactoe;framework-sample-play"
    zlink::framework_locations_redis
    zlink::framework
    zlink::stream_connector)
  add_executable(sample_cpp_framework_tictactoe_client
    samples/TicTacToe/Client/main.cpp)
  target_link_libraries(sample_cpp_framework_tictactoe_client PRIVATE
    zlink::http_client
    zlink::stream_e2e_client)
  add_zlink_framework_sample(
    sample_cpp_framework_deliverydispatch_dispatch
    samples/DeliveryDispatch/Server/Dispatch/main.cpp
    "framework-sample-deliverydispatch;framework-sample-api;framework-sample-dispatch"
    zlink::framework
    zlink::framework_locations_redis
    zlink::http_client)
  add_zlink_framework_sample(
    sample_cpp_framework_deliverydispatch_courier_actor_node
    samples/DeliveryDispatch/Server/CourierActorNode/main.cpp
    "framework-sample-deliverydispatch;framework-sample-courier"
    zlink::framework
    zlink::framework_locations_redis)
  add_zlink_framework_sample(
    sample_cpp_framework_deliverydispatch_customer_gateway
    samples/DeliveryDispatch/Server/CustomerGateway/main.cpp
    "framework-sample-deliverydispatch;framework-sample-session"
    zlink::framework
    zlink::framework_locations_redis)
  add_zlink_framework_sample(
    sample_cpp_framework_deliverydispatch_courier_session
    samples/DeliveryDispatch/Server/CourierSession/main.cpp
    "framework-sample-deliverydispatch;framework-sample-session"
    zlink::framework
    zlink::framework_locations_redis)
  add_zlink_framework_sample(
    sample_cpp_framework_deliverydispatch_tracking
    samples/DeliveryDispatch/Server/Tracking/main.cpp
    "framework-sample-deliverydispatch;framework-sample-tracking"
    zlink::framework
    zlink::framework_locations_redis)
  add_executable(sample_cpp_framework_deliverydispatch_client
    samples/DeliveryDispatch/Client/main.cpp)
  target_link_libraries(sample_cpp_framework_deliverydispatch_client PRIVATE
    zlink::framework
    zlink::http_client
    zlink::stream_connector
    zlink::stream_e2e_client)
  add_zlink_framework_sample(
    sample_cpp_framework_gamequest_game_api
    samples/GameQuest/Server/GameApi/main.cpp
    "framework-sample-gamequest;framework-sample-api"
    zlink::framework
    zlink::framework_locations_redis
    zlink::http_client
    zlink::stream_connector)
  add_zlink_framework_sample(
    sample_cpp_framework_gamequest_quest_mission
    samples/GameQuest/Server/QuestMission/main.cpp
    "framework-sample-gamequest;framework-sample-mission"
    zlink::framework
    zlink::framework_locations_redis)
  add_executable(sample_cpp_framework_gamequest_client
    samples/GameQuest/Client/main.cpp)
  target_link_libraries(sample_cpp_framework_gamequest_client PRIVATE
    zlink::framework
    zlink::http_client
    zlink::stream_connector
    zlink::stream_e2e_client)
  add_zlink_framework_sample(
    sample_cpp_framework_shoppingmall_commerce_api
    samples/ShoppingMall/Server/CommerceApi/main.cpp
    "framework-sample-shoppingmall;framework-sample-api"
    zlink::framework
    zlink::framework_locations_redis
    zlink::http_client)
  add_zlink_framework_sample(
    sample_cpp_framework_shoppingmall_order_workflow
    samples/ShoppingMall/Server/OrderWorkflow/main.cpp
    "framework-sample-shoppingmall;framework-sample-workflow"
    zlink::framework
    zlink::framework_locations_redis)
  add_executable(sample_cpp_framework_shoppingmall_client
    samples/ShoppingMall/Client/main.cpp)
  target_link_libraries(sample_cpp_framework_shoppingmall_client PRIVATE
    zlink::http_client)
  add_zlink_framework_sample(
    sample_cpp_framework_supportchat_api
    samples/SupportChat/Server/Api/main.cpp
    "framework-sample-supportchat;framework-sample-api"
    zlink::framework
    zlink::framework_locations_redis)
  add_zlink_framework_sample(
    sample_cpp_framework_supportchat_session
    samples/SupportChat/Server/Session/main.cpp
    "framework-sample-supportchat;framework-sample-session"
    zlink::framework
    zlink::framework_locations_redis
    zlink::stream_connector)
  add_zlink_framework_sample(
    sample_cpp_framework_supportchat_support
    samples/SupportChat/Server/Support/main.cpp
    "framework-sample-supportchat;framework-sample-support"
    zlink::framework
    zlink::framework_locations_redis)
  add_executable(sample_cpp_framework_supportchat_client
    samples/SupportChat/Client/main.cpp)
  target_link_libraries(sample_cpp_framework_supportchat_client PRIVATE
    zlink::framework
    zlink::http_client
    zlink::stream_connector
    zlink::stream_e2e_client)

  function(add_zlink_framework_sample_runner sample_name labels timeout_seconds)
    add_test(
      NAME sample_smoke_sample_cpp_framework_${sample_name}
      COMMAND "${ZLINK_FRAMEWORK_CPP_DIR}/samples/${sample_name}/run_sample.sh")
    set_tests_properties(sample_smoke_sample_cpp_framework_${sample_name} PROPERTIES
      LABELS "${labels};framework-sample-smoke"
      RESOURCE_LOCK "zlink-cpp-sample-runtime"
      TIMEOUT ${timeout_seconds})
  endfunction()

  add_zlink_framework_sample_runner(
    Bingo
    "framework-sample-bingo;framework-sample-api;framework-sample-play;framework-sample-session"
    180)
  add_zlink_framework_sample_runner(
    TicTacToe
    "framework-sample-tictactoe;framework-sample-api;framework-sample-play"
    180)
  add_zlink_framework_sample_runner(
    DeliveryDispatch
    "framework-sample-deliverydispatch;framework-sample-api;framework-sample-courier;framework-sample-dispatch;framework-sample-registry;framework-sample-session;framework-sample-tracking"
    420)
  add_zlink_framework_sample_runner(
    GameQuest
    "framework-sample-gamequest;framework-sample-api;framework-sample-mission"
    300)
  add_zlink_framework_sample_runner(
    ShoppingMall
    "framework-sample-shoppingmall;framework-sample-api;framework-sample-workflow"
    300)
  add_zlink_framework_sample_runner(
    SupportChat
    "framework-sample-supportchat;framework-sample-api;framework-sample-session;framework-sample-support"
    240)
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_TESTS AND ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE)
  find_program(ZLINK_GCOV_EXECUTABLE gcov REQUIRED)
  get_property(ZLINK_FRAMEWORK_CPP_TESTS DIRECTORY PROPERTY TESTS)
  add_test(
    NAME test_cpp_framework_coverage_threshold
    COMMAND ${CMAKE_COMMAND}
      -D ZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -D ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
      -D ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD=${ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD}
      -D ZLINK_GCOV_EXECUTABLE=${ZLINK_GCOV_EXECUTABLE}
      -P ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.Coverage/coverage_threshold.cmake)
  set_tests_properties(test_cpp_framework_coverage_threshold PROPERTIES
    LABELS "framework-coverage;framework-regression"
    DEPENDS "${ZLINK_FRAMEWORK_CPP_TESTS}"
    TIMEOUT 120)
endif()
