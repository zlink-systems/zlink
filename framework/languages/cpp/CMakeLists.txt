cmake_minimum_required(VERSION 3.20)
project(zlink_framework_cpp VERSION 0.25.0 LANGUAGES C CXX)

# IDE project trees (Visual Studio, Xcode): group targets into solution
# folders via the FOLDER target property instead of leaving them flat.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(ZLINK_FRAMEWORK_CPP_SHARED
  "Build the distributable Framework C++ libraries as shared libraries" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Source builds retain their historical hidden-by-default executable policy.
  # The prebuilt shared libraries deliberately keep Framework-owned symbols at
  # default visibility; their ELF export surface is restricted per target below.
  if(NOT ZLINK_FRAMEWORK_CPP_SHARED)
    add_compile_options(
      $<$<COMPILE_LANGUAGE:CXX>:-fvisibility=hidden>
      $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
  else()
    # Keep out-of-line Framework definitions default-visible for the shared
    # package, but do not accidentally publish third-party header templates.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
  endif()
  add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-fno-gnu-unique>)
  if(UNIX AND NOT APPLE AND NOT ZLINK_FRAMEWORK_CPP_SHARED)
    add_link_options($<$<LINK_LANGUAGE:CXX>:-Wl,--exclude-libs,ALL>)
  endif()
  if(UNIX AND NOT APPLE AND NOT ZLINK_FRAMEWORK_CPP_SHARED
      AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/cmake/hide-executable-symbols.map")
    add_link_options(
      $<$<LINK_LANGUAGE:CXX>:-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/cmake/hide-executable-symbols.map>)
  endif()
endif()

if(MSVC)
  add_compile_definitions(NOMINMAX)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

include(GNUInstallDirs)
include(FetchContent)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE "Build bindings/cpp from the workspace (explicit opt-in)" OFF)
option(ZLINK_FRAMEWORK_CPP_BUILD_TESTS "Build C++ framework tests" ON)
option(ZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS "Build isolated C++ service foundation tests" ON)
option(ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES "Build C++ framework samples (opt-in; samples/sample-build-common.sh enables it for each samples/<Sample>/run_sample.sh)" OFF)
option(ZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE "Build the C++ cross-language e2e host" ON)
option(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK "Build and install C++ framework and HTTP client package artifacts" ON)
option(ZLINK_FRAMEWORK_CPP_STAGE_STANDALONE_DEPENDENCIES
  "Stage Core and the C++ binding in a standalone Framework install" ON)
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS api)
endif()
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
# framework/include headers include <zlink/json_profile.hpp>; every target built here sees it.
include_directories(${ZLINK_FRAMEWORK_CPP_DIR}/common/include)
set(ZLINK_FRAMEWORK_CPP_REPO_ROOT "${ZLINK_FRAMEWORK_CPP_DIR}/../../.." CACHE PATH
  "Path to the zlink repository root")
# Release staging places the shared runtime inputs next to this CMake project.
if(EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime/protocol/generated/cpp/service_wire_constants.hpp")
  set(ZLINK_FRAMEWORK_CPP_RUNTIME_DIR "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/framework/runtime")
  set(_zlink_use_system_boost OFF)
else()
  set(ZLINK_FRAMEWORK_CPP_RUNTIME_DIR "${ZLINK_FRAMEWORK_CPP_DIR}/runtime")
  set(_zlink_use_system_boost ON)
endif()
if(NOT EXISTS "${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp/service_wire_constants.hpp"
    OR NOT EXISTS "${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp/service_wire_pilot_codec.hpp")
  message(FATAL_ERROR "Missing generated protocol headers; prepare the C++ source archive with cmake/prepare-source-archive.cmake")
endif()
option(ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST
  "Use packaged Boost instead of the repository's vendored Boost" ${_zlink_use_system_boost})
list(APPEND CMAKE_MODULE_PATH "${ZLINK_FRAMEWORK_CPP_DIR}/cmake")
set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT "$ENV{ZLINK_LOCAL_PACKAGE_ROOT}" CACHE PATH
  "Root directory containing local package outputs")
set(ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION "1.7.0" CACHE STRING
  "Exact local zlink_cpp package version consumed by the C++ framework")
set(ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION "1.7.0" CACHE STRING
  "Exact local zlink Core package version embedded by zlink_cpp")
# The binding and Core package prefixes have three sources, in this order: the
# two ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_*_PREFIX options, the repository's local
# package outputs under .artifacts when this tree is a checkout that has them,
# and otherwise wherever find_package() locates the packages (CMAKE_PREFIX_PATH,
# a vcpkg toolchain). A release archive built outside the repository takes the
# third route with no option at all.
if(NOT ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT)
  if(WIN32 AND EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/.artifacts/windows/install/zlink-cpp/${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION}")
    set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT
      "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/.artifacts/windows")
  elseif(EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/.artifacts/wsl/install/zlink-cpp/${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION}")
    set(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT
      "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/.artifacts/wsl")
  endif()
endif()
if(ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT)
  set(_zlink_default_cpp_prefix
    "${ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT}/install/zlink-cpp/${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION}")
  set(_zlink_default_core_prefix
    "${ZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT}/install/zlink-core/${ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION}")
else()
  set(_zlink_default_cpp_prefix "")
  set(_zlink_default_core_prefix "")
endif()
set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX "${_zlink_default_cpp_prefix}" CACHE PATH
  "Local zlink_cpp package install prefix (empty: wherever find_package locates it)")
set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX "${_zlink_default_core_prefix}" CACHE PATH
  "Local zlink Core package install prefix used by zlink_cpp (empty: wherever find_package locates it)")
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
if(NOT TARGET zlink::cpp)
  foreach(_zlink_prefix IN ITEMS
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}"
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}")
    if(_zlink_prefix)
      list(PREPEND CMAKE_PREFIX_PATH "${_zlink_prefix}")
    endif()
  endforeach()
  if(ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE)
    if(NOT EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/bindings/cpp/CMakeLists.txt")
      message(FATAL_ERROR "Bindings source mode requires a zlink repository checkout")
    endif()
    set(ZLINK_CPP_CORE_PACKAGE_PREFIX "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}" CACHE PATH
      "Core package used by the workspace binding")
    # The binding resolves Core in its subdirectory, while framework test
    # registration references the resulting imported target from this directory.
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
    add_subdirectory("${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/bindings/cpp"
      "${CMAKE_CURRENT_BINARY_DIR}/bindings-cpp")
    unset(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL)
  else()
    find_package(zlink_cpp ${ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION} EXACT CONFIG REQUIRED)
  endif()
endif()
# A package config lives at <prefix>/lib/cmake/<name> (archive installs) or
# <prefix>/share/<name> (vcpkg); either way the prefix is what remains.
function(zlink_framework_cpp_prefix_from_config_dir out_var config_dir)
  get_filename_component(_zlink_parent "${config_dir}" DIRECTORY)
  get_filename_component(_zlink_parent_name "${_zlink_parent}" NAME)
  if(_zlink_parent_name STREQUAL "cmake")
    get_filename_component(_zlink_parent "${_zlink_parent}" DIRECTORY)
  endif()
  get_filename_component(_zlink_prefix "${_zlink_parent}" DIRECTORY)
  set(${out_var} "${_zlink_prefix}" PARENT_SCOPE)
endfunction()
if(NOT ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX AND zlink_cpp_DIR)
  zlink_framework_cpp_prefix_from_config_dir(_zlink_found_cpp_prefix "${zlink_cpp_DIR}")
  set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX "${_zlink_found_cpp_prefix}" CACHE PATH
    "Local zlink_cpp package install prefix (found by find_package)" FORCE)
endif()
if(NOT ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX AND zlink_DIR)
  zlink_framework_cpp_prefix_from_config_dir(_zlink_found_core_prefix "${zlink_DIR}")
  set(ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX "${_zlink_found_core_prefix}" CACHE PATH
    "Local zlink Core package install prefix used by zlink_cpp (found by find_package)" FORCE)
endif()
if(WIN32)
  set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/bin/zlink.dll")
  set(ZLINK_FRAMEWORK_CPP_NATIVE_STATIC_RUNTIME
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/zlink.lib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.dylib")
  set(ZLINK_FRAMEWORK_CPP_NATIVE_STATIC_RUNTIME
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.a")
else()
  set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.so")
  set(ZLINK_FRAMEWORK_CPP_NATIVE_STATIC_RUNTIME
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.a")
endif()
# Linux and macOS link the Core runtime with an RPATH, so a binary this build
# produces resolves libzlink wherever it runs from. Windows has no RPATH: the
# loader looks next to the image and on PATH. vcpkg's app-local deployment
# already puts every vcpkg dependency next to each built executable, but Core's
# zlink.dll comes from the local package prefix instead, so nothing staged it.
# The only Windows path that worked was CTest, which prepends the Core prefix to
# PATH for the tests it launches; a sample started by its own run_sample.ps1 died
# with STATUS_DLL_NOT_FOUND before main, leaving an empty role log and a
# readiness timeout. Stage the runtime into the executable output directory so
# the build tree is self-contained however a binary in it is started.
if(WIN32 AND EXISTS "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME}")
  get_property(ZLINK_FRAMEWORK_CPP_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(ZLINK_FRAMEWORK_CPP_MULTI_CONFIG)
    set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_STAGE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")
  else()
    set(ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_STAGE_DIR "${CMAKE_BINARY_DIR}")
  endif()
  add_custom_target(zlink_framework_cpp_stage_native_runtime ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME}"
      "${ZLINK_FRAMEWORK_CPP_NATIVE_RUNTIME_STAGE_DIR}"
    COMMENT "Staging the Core runtime next to the build tree executables"
    VERBATIM)
endif()

endif()

set(ZLINK_FRAMEWORK_CPP_M6A_RUNTIME_SOURCES
  framework/src/runtime/backend/raw_dealer_port.cpp
  framework/src/runtime/backend/raw_route_port.cpp
  framework/src/runtime/foundation/operation_registry.cpp
  framework/src/runtime/dispatch/coroutine_executor.cpp
  framework/src/runtime/dispatch/offload_executor.cpp
  framework/src/runtime/execution/state_lane.cpp
  framework/src/runtime/messaging/call_facade_runtime.cpp
  framework/src/runtime/messaging/logical_multicast_runtime.cpp
  framework/src/runtime/diagnostics/runtime_observation.cpp
  framework/src/runtime/diagnostics/logging.cpp
  framework/src/runtime/diagnostics/monitoring_runtime.cpp
  framework/src/runtime/protocol/service_wire_codec.cpp
  framework/src/runtime/protocol/relocation_envelope_codec.cpp
  framework/src/runtime/mesh/raw_mesh_node_owner.cpp
  framework/src/runtime/mesh/service_liveness_registry.cpp
  framework/src/runtime/mesh/service_mailbox.cpp
  framework/src/runtime/mesh/service_topology_registry.cpp
  framework/src/runtime/locations/service_descriptor_registry.cpp
  framework/src/runtime/fanout/raw_fanout_owner.cpp
  framework/src/runtime/fanout/fanout_subscription.cpp
  framework/src/runtime/fanout/fanout_location_runtime.cpp
  framework/src/runtime/client_server/raw_client_server_owner.cpp
  framework/src/runtime/client_server/client_server_location_runtime.cpp
  framework/src/runtime/channels/channel_reply_writer.cpp
  framework/src/runtime/messaging/client_call_codec.cpp
  framework/src/runtime/messaging/envelope_codec.cpp
  framework/src/runtime/messaging/request_failure_mapper.cpp
  framework/src/runtime/codecs/serializer.cpp
  framework/src/runtime/diagnostics/flow_context.cpp)

set(ZLINK_FRAMEWORK_CPP_M6B_RUNTIME_SOURCES
  framework/src/runtime/stateful/public_host_runtime.cpp
  framework/src/runtime/stateful/raw_stateful_dispatch.cpp
  framework/src/runtime/stateful/stateful_object_runtime.cpp
  framework/src/runtime/stateful/stream_session_registry.cpp)

set(ZLINK_FRAMEWORK_CPP_M6C_RUNTIME_SOURCES
  framework/src/runtime/stateful/maintenance_runtime.cpp)

# The M6 runtime sources are compiled exactly once, into object libraries.
# Both the m6* stepping-stone archives and zlink_framework consume the same
# objects, which halves the compile count for these files without changing
# archive contents, link order, or the installed export set (zlink_framework
# stays the only exported target).
function(zlink_framework_cpp_configure_msvc_target target_name)
  if(MSVC)
    target_compile_options(${target_name} PUBLIC /bigobj)
  endif()
endfunction()

if(ZLINK_FRAMEWORK_CPP_SHARED)
  set(ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE SHARED)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
  set(ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE STATIC)
endif()
if(ZLINK_FRAMEWORK_CPP_SHARED AND WIN32)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
if(ZLINK_FRAMEWORK_CPP_SHARED AND APPLE)
  # Xcode 16.4's libc++ keeps C++20 stop_token behind this switch. Apply it
  # before creating the object libraries and export it to archive consumers.
  add_compile_options(-fexperimental-library)
  add_link_options(-fexperimental-library)
endif()

function(zlink_framework_cpp_configure_shared_library target_name)
  if(NOT ZLINK_FRAMEWORK_CPP_SHARED)
    return()
  endif()

  set_target_properties(${target_name} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    # Keep the linker input independent of its temporary build directory so
    # GNU's content-derived build ID remains reproducible after installation.
    BUILD_WITH_INSTALL_RPATH ON)
  if(WIN32)
    set_target_properties(${target_name} PROPERTIES
      MSVC_RUNTIME_LIBRARY MultiThreadedDLL
      CXX_LINKER_LAUNCHER
        "${Python3_EXECUTABLE};${CMAKE_CURRENT_LIST_DIR}/cmake/filter-windows-exports.py")
  elseif(UNIX AND NOT APPLE)
    set_property(TARGET ${target_name} PROPERTY INSTALL_RPATH "$ORIGIN")
    target_link_options(${target_name} PRIVATE
      "-Wl,--exclude-libs,ALL"
      "-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/cmake/zlink-framework-shared-symbols.map")
  elseif(APPLE)
    set(_zlink_apple_exports
      "${CMAKE_CURRENT_BINARY_DIR}/zlink-framework-exported-symbols.list")
    get_property(_zlink_apple_exports_generated GLOBAL
      PROPERTY ZLINK_FRAMEWORK_CPP_APPLE_EXPORTS_GENERATED)
    if(NOT _zlink_apple_exports_generated)
      file(REMOVE "${_zlink_apple_exports}")
      file(STRINGS
        "${CMAKE_CURRENT_LIST_DIR}/cmake/zlink-framework-shared-symbols.map"
        _zlink_apple_export_lines REGEX "^# APPLE_EXPORT: ")
      foreach(_zlink_apple_export_line IN LISTS _zlink_apple_export_lines)
        string(REGEX REPLACE "^# APPLE_EXPORT: " ""
          _zlink_apple_export "${_zlink_apple_export_line}")
        file(APPEND "${_zlink_apple_exports}" "${_zlink_apple_export}\n")
      endforeach()
      set_property(GLOBAL PROPERTY
        ZLINK_FRAMEWORK_CPP_APPLE_EXPORTS_GENERATED TRUE)
    endif()
    set_target_properties(${target_name} PROPERTIES
      INSTALL_NAME_DIR "@loader_path"
      INSTALL_RPATH "@loader_path")
    target_link_options(${target_name} PRIVATE
      "-Wl,-exported_symbols_list,${_zlink_apple_exports}")
  endif()
endfunction()

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
add_library(zlink_framework_provider_abstractions ${ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE}
  framework/src/runtime/dispatch/provider_task_context.cpp)
add_library(zlink::framework_provider_abstractions ALIAS
  zlink_framework_provider_abstractions)
set_target_properties(zlink_framework_provider_abstractions PROPERTIES
  EXPORT_NAME framework_provider_abstractions)
zlink_framework_cpp_configure_shared_library(zlink_framework_provider_abstractions)
target_compile_features(zlink_framework_provider_abstractions PUBLIC cxx_std_20)
target_include_directories(zlink_framework_provider_abstractions PUBLIC
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/framework/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

function(zlink_framework_cpp_configure_m6_objects target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  target_include_directories(${target_name} PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(${target_name} PRIVATE
    opentelemetry-cpp::api
    zlink::cpp
    Threads::Threads)
  zlink_framework_cpp_configure_msvc_target(${target_name})
endfunction()

add_library(zlink_framework_m6a_objects OBJECT
  ${ZLINK_FRAMEWORK_CPP_M6A_RUNTIME_SOURCES})
zlink_framework_cpp_configure_m6_objects(zlink_framework_m6a_objects)

add_library(zlink_framework_m6b_objects OBJECT
  ${ZLINK_FRAMEWORK_CPP_M6B_RUNTIME_SOURCES})
zlink_framework_cpp_configure_m6_objects(zlink_framework_m6b_objects)

add_library(zlink_framework_m6c_objects OBJECT
  ${ZLINK_FRAMEWORK_CPP_M6C_RUNTIME_SOURCES})
zlink_framework_cpp_configure_m6_objects(zlink_framework_m6c_objects)

add_library(zlink_framework_m6a_runtime STATIC
  $<TARGET_OBJECTS:zlink_framework_m6a_objects>)
target_compile_features(zlink_framework_m6a_runtime PUBLIC cxx_std_20)
target_include_directories(zlink_framework_m6a_runtime PUBLIC
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include)
target_link_libraries(zlink_framework_m6a_runtime PUBLIC
  opentelemetry-cpp::api
  zlink::cpp
  Threads::Threads)

add_library(zlink_framework_m6b_runtime STATIC
  $<TARGET_OBJECTS:zlink_framework_m6b_objects>)
target_compile_features(zlink_framework_m6b_runtime PUBLIC cxx_std_20)
target_link_libraries(zlink_framework_m6b_runtime PUBLIC
  zlink_framework_m6a_runtime
  Threads::Threads)

add_library(zlink_framework_m6c_runtime STATIC
  $<TARGET_OBJECTS:zlink_framework_m6c_objects>)
target_compile_features(zlink_framework_m6c_runtime PUBLIC cxx_std_20)
target_link_libraries(zlink_framework_m6c_runtime PUBLIC
  zlink_framework_m6b_runtime
  Threads::Threads)

add_library(zlink_framework ${ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE}
  $<TARGET_OBJECTS:zlink_framework_m6a_objects>
  $<TARGET_OBJECTS:zlink_framework_m6b_objects>
  $<TARGET_OBJECTS:zlink_framework_m6c_objects>
  framework/src/runtime/channels/channel_packet_dispatcher.cpp
  framework/src/runtime/channels/channel_host_service.cpp
  framework/src/runtime/channels/channel_bundle_factory.cpp
  framework/src/runtime/channels/channel_pending_requests.cpp
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
  framework/src/runtime/actors/actor_gateway_runtime.cpp
  framework/src/runtime/actors/actor_client.cpp
  framework/src/runtime/configuration/builders/configuration_builder.cpp
  framework/src/runtime/configuration/endpoint_connections.cpp
  framework/src/runtime/configuration/services.cpp
  framework/src/runtime/diagnostics/health.cpp
  framework/src/runtime/execution/serial_execution_queue.cpp
  framework/src/runtime/handlers/handler_registry.cpp
  framework/src/runtime/dispatch/projection.cpp
  framework/src/runtime/host/app.cpp
  framework/src/runtime/host/relocation_target_eligibility.cpp
  framework/src/runtime/http/http_listener.cpp
  framework/src/runtime/http/http_request_pipeline.cpp
  framework/src/runtime/host/framework_runtime.cpp
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
  framework/src/runtime/utils/uuid.cpp
  framework/src/runtime/transport/endpoint.cpp)
add_library(zlink::framework ALIAS zlink_framework)
set_target_properties(zlink_framework PROPERTIES EXPORT_NAME framework)
zlink_framework_cpp_configure_shared_library(zlink_framework)
target_compile_features(zlink_framework PUBLIC cxx_std_20)
if(ZLINK_FRAMEWORK_CPP_SHARED AND APPLE)
  target_compile_options(zlink_framework INTERFACE -fexperimental-library)
  target_link_options(zlink_framework INTERFACE -fexperimental-library)
endif()
zlink_framework_cpp_configure_msvc_target(zlink_framework)
if(MSVC)
  set_property(SOURCE framework/src/runtime/spots/spot_runtime.cpp APPEND PROPERTY
    COMPILE_OPTIONS
      "$<$<CONFIG:Release>:/O1>"
      "$<$<CONFIG:Release>:/Oi->")
endif()
target_include_directories(zlink_framework PUBLIC
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/framework/include>
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/common/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(zlink_framework PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
  ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
target_link_libraries(zlink_framework PUBLIC zlink::cpp zlink::framework_provider_abstractions)
if(ZLINK_FRAMEWORK_CPP_SHARED)
  # C++ exports metrics through runtime.status(), not an OpenTelemetry provider API.
  # Adding a provider-injection boundary would therefore be a spec change.
  target_link_libraries(zlink_framework PRIVATE opentelemetry-cpp::api)
else()
  target_link_libraries(zlink_framework PUBLIC opentelemetry-cpp::api)
endif()
if(WIN32)
  target_link_libraries(zlink_framework PUBLIC bcrypt)
endif()
endif()

set(ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR "" CACHE PATH
  "Boost include directory used by the C++ framework and stream connector")
if(NOT ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST AND NOT ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR
    AND EXISTS "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/external/boost/boost/asio.hpp")
  set(ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR
    "${ZLINK_FRAMEWORK_CPP_REPO_ROOT}/core/external/boost" CACHE PATH
    "Boost include directory used by the C++ framework and stream connector" FORCE)
endif()
if(ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST OR NOT ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR)
  find_package(Boost REQUIRED)
endif()
function(zlink_framework_cpp_add_boost_headers target_name)
  if(NOT ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST AND ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR)
    # Keep the bundled Boost tree ahead of transitive package include paths. A
    # framework target and the standalone HTTP client are linked into the same
    # consumer; selecting different Asio headers in those targets is an ABI
    # violation even though the headers are nominally header-only.
    target_include_directories(${target_name} BEFORE PRIVATE
      "${ZLINK_FRAMEWORK_CPP_BOOST_INCLUDE_DIR}")
  elseif(TARGET Boost::headers)
    target_link_libraries(${target_name} PRIVATE Boost::headers)
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

# Boost.Asio owns the socket implementation and keeps the runtime free of
# platform-specific transport branches. Windows still requires the Winsock
# libraries at final link time, including when a static connector is consumed
# by an engine module.
function(zlink_framework_cpp_add_asio_system_libraries target_name)
  if(WIN32)
    target_link_libraries(${target_name} PRIVATE ws2_32 mswsock)
  endif()
endfunction()

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
zlink_framework_cpp_add_boost_headers(zlink_framework)
# The M6 objects are linked into zlink_framework, so they must see the same
# Boost headers as the rest of its translation units.
zlink_framework_cpp_add_boost_headers(zlink_framework_m6a_objects)
zlink_framework_cpp_add_boost_headers(zlink_framework_m6b_objects)
zlink_framework_cpp_add_boost_headers(zlink_framework_m6c_objects)
zlink_framework_cpp_add_asio_system_libraries(zlink_framework)
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()
find_package(OpenSSL QUIET)
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
# These object libraries compile Framework headers before they are consumed by
# zlink_framework, so they need the JSON include usage requirement directly.
foreach(M6ObjectTarget IN ITEMS
    zlink_framework_m6a_objects
    zlink_framework_m6b_objects
    zlink_framework_m6c_objects)
  target_link_libraries(${M6ObjectTarget} PRIVATE nlohmann_json::nlohmann_json)
endforeach()
target_link_libraries(zlink_framework PUBLIC nlohmann_json::nlohmann_json)
if(OpenSSL_FOUND)
  target_compile_definitions(zlink_framework PRIVATE
    ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
    ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL)
  target_link_libraries(zlink_framework PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

endif()

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
add_library(zlink_http_client ${ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE}
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
zlink_framework_cpp_configure_shared_library(zlink_http_client)
target_compile_features(zlink_http_client PUBLIC cxx_std_20)
zlink_framework_cpp_configure_msvc_target(zlink_http_client)
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
zlink_framework_cpp_add_asio_system_libraries(zlink_http_client)
if(OpenSSL_FOUND)
  target_compile_definitions(zlink_http_client PRIVATE
    ZLINK_HTTP_CLIENT_WITH_OPENSSL)
  target_link_libraries(zlink_http_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  set(ZLINK_HTTP_CLIENT_EXPORT_OPENSSL_DEPENDENCY
    "find_dependency(OpenSSL)")
else()
  set(ZLINK_HTTP_CLIENT_EXPORT_OPENSSL_DEPENDENCY "")
endif()
# The HTTPS test certificate is only needed when the http-client-https contract test is
# actually going to be built (see test_cpp_http_client below, which is registered only under
# ZLINK_FRAMEWORK_CPP_BUILD_TESTS). Tying this block to the same condition means the rule reads
# as one sentence: if the HTTPS contract tests are being built, the openssl CLI and a generated
# certificate are required. A library consumer configuring with
# ZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF must not need the openssl CLI at all.
if(ZLINK_FRAMEWORK_CPP_BUILD_TESTS)
  find_program(ZLINK_OPENSSL_EXECUTABLE openssl)
  # The http-client-https ctest label is unconditionally required by
  # verify_ctest_label_contract.cmake whenever tests are built, so once the OpenSSL library is
  # available (HTTPS client support is being compiled in), the openssl CLI and a successfully
  # generated test certificate are hard requirements, not best-effort conveniences. Failing here
  # with the real cause (missing tool, or the tool failed) is more legible than warning and
  # continuing: continuing left the label contract test to fail several steps later with "label
  # http-client-https selects no tests", which reads like a label-taxonomy bug rather than a
  # missing local tool.
  if(OpenSSL_FOUND)
    if(NOT ZLINK_OPENSSL_EXECUTABLE)
      message(FATAL_ERROR
        "OpenSSL library was found (HTTPS client support is being built), but the 'openssl' "
        "command-line executable is not on PATH. It is required to generate the test "
        "certificate used by the http-client-https contract tests. Install the OpenSSL CLI "
        "(it ships with most OpenSSL distributions, including vcpkg's openssl port) and make "
        "sure it is on PATH, then re-run configure.")
    endif()
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
      OUTPUT_VARIABLE ZLINK_HTTP_CLIENT_CERT_OUTPUT
      ERROR_VARIABLE ZLINK_HTTP_CLIENT_CERT_ERROR)
    if(NOT ZLINK_HTTP_CLIENT_CERT_RESULT EQUAL 0
        OR NOT EXISTS "${ZLINK_HTTP_CLIENT_TEST_CERT}"
        OR NOT EXISTS "${ZLINK_HTTP_CLIENT_TEST_KEY}")
      message(FATAL_ERROR
        "Failed to generate the HTTP client HTTPS test certificate with "
        "'${ZLINK_OPENSSL_EXECUTABLE}' (exit code ${ZLINK_HTTP_CLIENT_CERT_RESULT}). "
        "The http-client-https contract tests require this certificate.\n"
        "openssl stdout:\n${ZLINK_HTTP_CLIENT_CERT_OUTPUT}\n"
        "openssl stderr:\n${ZLINK_HTTP_CLIENT_CERT_ERROR}")
    endif()
    set(ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL ON)
  endif()
endif()

endif()

add_library(zlink_stream_connector ${ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE}
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
zlink_framework_cpp_configure_shared_library(zlink_stream_connector)
target_compile_features(zlink_stream_connector PUBLIC cxx_std_20)
zlink_framework_cpp_configure_msvc_target(zlink_stream_connector)
target_include_directories(zlink_stream_connector PUBLIC
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/include>
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/common/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(zlink_stream_connector PRIVATE
  ${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/src)
zlink_framework_cpp_add_boost_headers(zlink_stream_connector)
zlink_framework_cpp_add_asio_system_libraries(zlink_stream_connector)
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
if(ZLINK_STREAM_CONNECTOR_WITH_LZ4
    OR (ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK AND ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4))
  find_package(lz4 REQUIRED)
  if(ZLINK_STREAM_CONNECTOR_WITH_LZ4)
    target_link_libraries(zlink_stream_connector PRIVATE lz4::lz4)
    set(ZLINK_STREAM_CONNECTOR_EXPORT_LZ4_DEPENDENCY "find_dependency(lz4)")
  endif()
  if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK AND ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4)
    target_link_libraries(zlink_framework PRIVATE lz4::lz4)
    set(ZLINK_FRAMEWORK_EXPORT_LZ4_DEPENDENCY "find_dependency(lz4)")
  endif()
endif()
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  target_compile_definitions(zlink_framework PRIVATE
    $<$<BOOL:${ZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4}>:ZLINK_FRAMEWORK_STREAM_WITH_LZ4>)
endif()
target_compile_definitions(zlink_stream_connector PRIVATE
  $<$<BOOL:${ZLINK_STREAM_CONNECTOR_WITH_LZ4}>:ZLINK_STREAM_CONNECTOR_WITH_LZ4>)

add_library(zlink_stream_connector_codecs INTERFACE)
add_library(zlink::stream_connector_codecs ALIAS zlink_stream_connector_codecs)
set_target_properties(zlink_stream_connector_codecs PROPERTIES
  EXPORT_NAME stream_connector_codecs)
target_compile_features(zlink_stream_connector_codecs INTERFACE cxx_std_20)
target_include_directories(zlink_stream_connector_codecs INTERFACE
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/framework/include>
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/common/include>
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
  zlink_framework_cpp_add_asio_system_libraries(zlink_unreal_stream_connector)
  if(ZLINK_STREAM_CONNECTOR_WITH_LZ4)
    target_link_libraries(zlink_unreal_stream_connector PRIVATE lz4::lz4)
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
  target_link_libraries(zlink_godot_stream_connector
    PUBLIC zlink::stream_connector
    PRIVATE zlink::stream_connector_codecs)
endif()

if(ZLINK_STREAM_CONNECTOR_BUILD_AXMOL)
  add_library(zlink_axmol_stream_connector STATIC
    connector/engines/axmol/src/zlink_axmol_stream_connector.cpp)
  target_compile_features(zlink_axmol_stream_connector PUBLIC cxx_std_20)
  target_include_directories(zlink_axmol_stream_connector PUBLIC
    $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/engines/axmol/include>)
  target_link_libraries(zlink_axmol_stream_connector
    PUBLIC zlink::stream_connector
    PRIVATE zlink::stream_connector_codecs)
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

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  zlink_enable_coverage(zlink_framework)
  zlink_enable_coverage(zlink_http_client)
endif()
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

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
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
find_package(protobuf CONFIG QUIET)
if(NOT protobuf_FOUND)
  find_package(Protobuf MODULE REQUIRED)
endif()
target_link_libraries(zlink_framework_codec_protobuf INTERFACE protobuf::libprotobuf)

# Generates protobuf sources for a sample/e2e schema and exposes them as a target.
function(add_zlink_protobuf_schema target_name proto_file)
  get_filename_component(proto_dir "${proto_file}" DIRECTORY)
  file(RELATIVE_PATH proto_relative_dir
    "${CMAKE_CURRENT_SOURCE_DIR}" "${proto_dir}")
  add_library(${target_name} STATIC "${proto_file}")
  target_link_libraries(${target_name} PUBLIC protobuf::libprotobuf)
  target_include_directories(${target_name} PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/${proto_relative_dir}")
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

# The Redis client (redis-plus-plus, hiredis, libuv) is private to this
# library: its public header names no third-party type, so the shared package
# embeds the client and its consumers link zlink::framework_locations_redis
# like the other Framework libraries.
add_library(zlink_framework_locations_redis ${ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE}
  extensions/framework-locations-redis/src/redis.cpp)
add_library(zlink::framework_locations_redis ALIAS zlink_framework_locations_redis)
set_target_properties(zlink_framework_locations_redis PROPERTIES
  EXPORT_NAME framework_locations_redis)
zlink_framework_cpp_configure_shared_library(zlink_framework_locations_redis)
zlink_framework_cpp_configure_msvc_target(zlink_framework_locations_redis)
target_compile_features(zlink_framework_locations_redis PUBLIC cxx_std_20)
target_include_directories(zlink_framework_locations_redis PUBLIC
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/extensions/framework-locations-redis/include>
  $<BUILD_INTERFACE:${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zlink_framework_locations_redis PUBLIC
  zlink::framework_provider_abstractions
  zlink::framework)
if(TARGET redis++::redis++)
  target_link_libraries(zlink_framework_locations_redis PRIVATE
    redis++::redis++)
  target_compile_definitions(zlink_framework_locations_redis PRIVATE
    ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT=1)
elseif(TARGET redis++::redis++_static)
  target_link_libraries(zlink_framework_locations_redis PRIVATE
    redis++::redis++_static)
  target_compile_definitions(zlink_framework_locations_redis PRIVATE
    ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT=1)
endif()
if(TARGET libuv::uv_a)
  target_link_libraries(zlink_framework_locations_redis PRIVATE
    libuv::uv_a)
elseif(TARGET libuv::uv)
  target_link_libraries(zlink_framework_locations_redis PRIVATE
    libuv::uv)
endif()

set(ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES "")
set(ZLINK_FRAMEWORK_REDIS_EXPORTS_REDIS_PLUS_PLUS OFF)
set(ZLINK_FRAMEWORK_REDIS_EXPORTS_LIBUV OFF)
if(TARGET redis++::redis++)
  set(ZLINK_FRAMEWORK_REDIS_EXPORTS_REDIS_PLUS_PLUS ON)
  string(APPEND ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES
    "find_dependency(redis++ CONFIG)\n")
elseif(TARGET redis++::redis++_static)
  set(ZLINK_FRAMEWORK_REDIS_EXPORTS_REDIS_PLUS_PLUS ON)
  string(APPEND ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES
    "find_dependency(redis++ CONFIG)\n")
endif()

if(TARGET libuv::uv_a)
  set(ZLINK_FRAMEWORK_REDIS_EXPORTS_LIBUV ON)
  string(APPEND ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES
    "find_dependency(libuv CONFIG)\n")
elseif(TARGET libuv::uv)
  set(ZLINK_FRAMEWORK_REDIS_EXPORTS_LIBUV ON)
  string(APPEND ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES
    "find_dependency(libuv CONFIG)\n")
endif()

endif()

set(ZLINK_FRAMEWORK_CPP_PACKAGE_DEPENDENCY_PREFIX_PATH
  ${CMAKE_PREFIX_PATH})
# Every dependency the installed package config resolves with
# find_dependency must be reachable from the consumer prefix path.
foreach(dependency_dir IN ITEMS
    "${redis++_DIR}" "${hiredis_DIR}" "${libuv_DIR}"
    "${opentelemetry-cpp_DIR}" "${nlohmann_json_DIR}" "${Protobuf_DIR}"
    "${protobuf_DIR}")
  if(dependency_dir)
    list(APPEND ZLINK_FRAMEWORK_CPP_PACKAGE_DEPENDENCY_PREFIX_PATH
      "${dependency_dir}")
  endif()
endforeach()

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(TARGETS
    zlink_framework
    zlink_framework_provider_abstractions
    zlink_framework_codec_messagepack
    EXPORT zlink_framework_cppTargets
    COMPONENT Framework)
  install(TARGETS
    zlink_framework_codec_protobuf
    EXPORT zlink_framework_cppCodecProtobufTargets
    COMPONENT Framework)
  install(TARGETS
    zlink_framework_locations_redis
    EXPORT zlink_framework_cppLocationsRedisTargets
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
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK
    AND ZLINK_FRAMEWORK_CPP_STAGE_STANDALONE_DEPENDENCIES
    AND NOT ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE)
  # The server Framework targets still use zlink::cpp. Preserve each dependency
  # config's original depth so its generated imported paths resolve in the archive.
  function(zlink_framework_cpp_stage_dependency_config found_dir)
    get_filename_component(_parent "${found_dir}" DIRECTORY)
    get_filename_component(_parent_name "${_parent}" NAME)
    if(_parent_name STREQUAL "cmake")
      get_filename_component(_grandparent "${_parent}" DIRECTORY)
      get_filename_component(_grandparent_name "${_grandparent}" NAME)
      set(_destination "${_grandparent_name}/cmake")
    else()
      set(_destination "${_parent_name}")
    endif()
    install(DIRECTORY "${found_dir}"
      DESTINATION "${_destination}" COMPONENT Framework)
  endfunction()

  function(zlink_framework_cpp_stage_debug_counterparts prefix found_files)
    foreach(_file IN LISTS found_files)
      file(RELATIVE_PATH _relative "${prefix}" "${_file}")
      set(_debug_file "${prefix}/debug/${_relative}")
      if(EXISTS "${_debug_file}")
        get_filename_component(_relative_dir "${_relative}" DIRECTORY)
        install(FILES "${_debug_file}"
          DESTINATION "debug/${_relative_dir}" COMPONENT Framework)
      endif()
    endforeach()
  endfunction()

  install(DIRECTORY "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/include/"
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Framework FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
  file(GLOB _binding_libraries
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/libzlink_cpp.a"
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/zlink_cpp.lib"
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/libzlink_cpp.lib"
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/libzlink_cpp.dll.a")
  if(WIN32)
    file(GLOB _binding_import_libraries
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/*.lib"
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/lib/*.dll.a")
    list(APPEND _binding_libraries ${_binding_import_libraries})
    list(REMOVE_DUPLICATES _binding_libraries)
  endif()
  if(NOT _binding_libraries)
    message(FATAL_ERROR "The installed zlink_cpp package has no link library")
  endif()
  install(FILES ${_binding_libraries}
    DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Framework)
  zlink_framework_cpp_stage_debug_counterparts(
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}" "${_binding_libraries}")
  if(NOT zlink_cpp_DIR OR NOT zlink_DIR)
    message(FATAL_ERROR "The Framework archive needs findable zlink_cpp and zlink configs")
  endif()
  zlink_framework_cpp_stage_dependency_config("${zlink_cpp_DIR}")
  zlink_framework_cpp_stage_dependency_config("${zlink_DIR}")

  if(WIN32)
    install(DIRECTORY "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/bin/"
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT Framework FILES_MATCHING PATTERN "*.dll")
    file(GLOB _core_libraries
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/*.lib"
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/bin/*.lib")
  elseif(APPLE)
    file(GLOB _core_libraries
      # Core's macOS archive carries its OpenSSL dylibs next to libzlink (@loader_path).
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/*.dylib"
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.a")
  else()
    file(GLOB _core_libraries
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.so*"
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/lib/libzlink.a")
  endif()
  if(NOT _core_libraries)
    message(FATAL_ERROR "The installed Core package has no link library")
  endif()
  install(FILES ${_core_libraries}
    DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Framework)
  zlink_framework_cpp_stage_debug_counterparts(
    "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}" "${_core_libraries}")
endif()
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(DIRECTORY common/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT Framework
    FILES_MATCHING PATTERN "*.hpp")
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
install(DIRECTORY common/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT StreamConnector
  FILES_MATCHING PATTERN "*.hpp")
install(FILES
  framework/include/zlink/framework/codecs/json_stream_connector.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zlink/framework/codecs
  COMPONENT StreamConnector)
if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)
  install(FILES framework/include/zlink/framework/codecs/json_stream_e2e_client.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zlink/framework/codecs
    COMPONENT StreamConnector)
endif()
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
if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  install(EXPORT zlink_framework_cppTargets
    NAMESPACE zlink::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_framework_cpp
    COMPONENT Framework)
  install(EXPORT zlink_framework_cppCodecProtobufTargets
    NAMESPACE zlink::
    FILE zlink_framework_cppCodecProtobufTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_framework_cpp
    COMPONENT Framework)
  install(EXPORT zlink_framework_cppLocationsRedisTargets
    NAMESPACE zlink::
    FILE zlink_framework_cppLocationsRedisTargets.cmake
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

set(ZLINK_FRAMEWORK_CPP_CONFIG_IMPLEMENTATION_DEPENDENCIES "")
set(ZLINK_HTTP_CLIENT_CONFIG_IMPLEMENTATION_DEPENDENCIES "")
set(ZLINK_STREAM_CONNECTOR_CONFIG_IMPLEMENTATION_DEPENDENCIES "")
set(ZLINK_FRAMEWORK_CONFIG_IMPLEMENTATION_DEPENDENCIES "")
if(NOT ZLINK_FRAMEWORK_CPP_SHARED)
  string(CONCAT ZLINK_FRAMEWORK_CPP_CONFIG_IMPLEMENTATION_DEPENDENCIES
    "find_dependency(opentelemetry-cpp CONFIG COMPONENTS api)\n"
    "find_dependency(Boost)\n"
    "find_package(protobuf CONFIG QUIET)\n"
    "if(NOT protobuf_FOUND)\n"
    "  find_dependency(Protobuf MODULE)\n"
    "endif()\n"
    "${ZLINK_HTTP_CLIENT_EXPORT_OPENSSL_DEPENDENCY}\n"
    "${ZLINK_FRAMEWORK_REDIS_EXPORT_DEPENDENCIES}"
    "set(_zlink_saved_module_path \"\${CMAKE_MODULE_PATH}\")\n"
    "list(APPEND CMAKE_MODULE_PATH \"\${CMAKE_CURRENT_LIST_DIR}\")\n"
    "${ZLINK_FRAMEWORK_EXPORT_LZ4_DEPENDENCY}\n"
    "set(CMAKE_MODULE_PATH \"\${_zlink_saved_module_path}\")\n"
    "unset(_zlink_saved_module_path)")
  string(CONCAT ZLINK_HTTP_CLIENT_CONFIG_IMPLEMENTATION_DEPENDENCIES
    "find_dependency(Boost)\n"
    "${ZLINK_HTTP_CLIENT_EXPORT_OPENSSL_DEPENDENCY}")
  string(CONCAT ZLINK_STREAM_CONNECTOR_CONFIG_IMPLEMENTATION_DEPENDENCIES
    "find_dependency(Boost)\n"
    "${ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY}\n"
    "set(_zlink_saved_module_path \"\${CMAKE_MODULE_PATH}\")\n"
    "list(APPEND CMAKE_MODULE_PATH \"\${CMAKE_CURRENT_LIST_DIR}\")\n"
    "${ZLINK_STREAM_CONNECTOR_EXPORT_LZ4_DEPENDENCY}\n"
    "set(CMAKE_MODULE_PATH \"\${_zlink_saved_module_path}\")\n"
    "unset(_zlink_saved_module_path)")
  string(CONCAT ZLINK_FRAMEWORK_CONFIG_IMPLEMENTATION_DEPENDENCIES
    "find_package(protobuf CONFIG QUIET)\n"
    "if(NOT protobuf_FOUND)\n"
    "  find_dependency(Protobuf)\n"
    "endif()\n"
    "find_dependency(Boost)")
else()
  set(ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY "")
endif()

foreach(_package zlink_framework_cpp zlink_stream_connector_cpp)
  if(_package STREQUAL "zlink_framework_cpp" AND NOT ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
    continue()
  endif()
  if(_package STREQUAL "zlink_framework_cpp")
    set(_component Framework)
  else()
    set(_component StreamConnector)
  endif()
  install(FILES "${CMAKE_CURRENT_LIST_DIR}/cmake/Findlz4.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${_package} COMPONENT ${_component})
endforeach()

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

if(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK)
  configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/zlink_frameworkConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zlink_frameworkConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_framework)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/zlink_frameworkConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/zlink_frameworkConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/zlink_frameworkConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlink_framework COMPONENT Framework)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS OR ZLINK_FRAMEWORK_CPP_BUILD_TESTS
    OR ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES)
  include(${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.ContractTests/release_test_labels.cmake)
endif()

function(zlink_framework_cpp_add_test)
  set(one_value_args NAME LABELS TIER TIMEOUT)
  set(multi_value_args COMMAND ENVIRONMENT PROPERTIES)
  cmake_parse_arguments(PARSE_ARGV 0 ZLINK_TEST "" "${one_value_args}" "${multi_value_args}")
  if(NOT ZLINK_TEST_NAME OR NOT ZLINK_TEST_COMMAND OR
      NOT ZLINK_TEST_LABELS OR NOT DEFINED ZLINK_TEST_TIMEOUT OR
      ZLINK_TEST_UNPARSED_ARGUMENTS OR ZLINK_TEST_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "Invalid zlink framework CTest registration: ${ARGV}")
  endif()

  add_test(NAME "${ZLINK_TEST_NAME}" COMMAND ${ZLINK_TEST_COMMAND})
  set(test_environment ${ZLINK_TEST_ENVIRONMENT})
  if(WIN32)
    set(test_path
      "${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}/bin;${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX}/bin;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin;$ENV{PATH}")
    string(REPLACE ";" "\\;" test_path "${test_path}")
    list(PREPEND test_environment "PATH=${test_path}")
  endif()
  set(test_labels ${ZLINK_TEST_LABELS})
  if(ZLINK_TEST_TIER)
    list(APPEND test_labels "${ZLINK_TEST_TIER}")
  else()
    foreach(tier IN LISTS ZLINK_FRAMEWORK_CPP_TEST_TIERS)
      foreach(prefix framework connector http-client)
        list(FIND test_labels "${prefix}-${tier}" tier_index)
        if(NOT tier_index EQUAL -1)
          list(APPEND test_labels "${tier}")
          break()
        endif()
      endforeach()
    endforeach()
  endif()
  set_tests_properties("${ZLINK_TEST_NAME}" PROPERTIES
    LABELS "${test_labels}"
    TIMEOUT "${ZLINK_TEST_TIMEOUT}")
  if(test_environment)
    set_property(TEST "${ZLINK_TEST_NAME}" PROPERTY ENVIRONMENT "${test_environment}")
  endif()
  if(ZLINK_TEST_PROPERTIES)
    set_tests_properties("${ZLINK_TEST_NAME}" PROPERTIES ${ZLINK_TEST_PROPERTIES})
  endif()
endfunction()

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

if(ZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS OR ZLINK_FRAMEWORK_CPP_BUILD_TESTS)
  find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS sdk)
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS)
  add_executable(test_cpp_framework_raw_route_port_contract
    tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp
    framework/src/runtime/backend/raw_route_port.cpp)
  target_compile_features(test_cpp_framework_raw_route_port_contract PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_raw_route_port_contract PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  target_link_libraries(test_cpp_framework_raw_route_port_contract PRIVATE
    zlink_framework
    zlink::cpp
    Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_raw_route_port_contract)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_raw_route_port_contract
    COMMAND test_cpp_framework_raw_route_port_contract
    LABELS "framework-contract;framework-unit;framework-foundation;raw-binding"
    TIER unit
    TIMEOUT 30)

  add_executable(test_cpp_framework_service_wire_codec
    tests/Zlink.Framework.UnitTests/test_cpp_framework_service_wire_codec.cpp
    framework/src/runtime/protocol/service_wire_codec.cpp
    framework/src/runtime/messaging/envelope_codec.cpp
    framework/src/runtime/codecs/serializer.cpp
    framework/src/runtime/diagnostics/flow_context.cpp
    framework/src/runtime/dispatch/offload_executor.cpp
    framework/src/runtime/execution/state_lane.cpp
    framework/src/runtime/mesh/service_topology_registry.cpp)
  target_compile_features(test_cpp_framework_service_wire_codec PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_service_wire_codec PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_compile_definitions(test_cpp_framework_service_wire_codec PRIVATE
    ZLINK_BOUND_SESSION_REPLACED_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/bound-session-replaced-v1.json"
    ZLINK_REPLY_RELAY_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/reply-relay-v1.json"
    ZLINK_RELOCATION_CONTROL_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/relocation-control-v1.json"
    ZLINK_SESSION_RELOCATION_BARRIER_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/session-relocation-barrier-v1.json"
    ZLINK_USER_SPOT_CREATE_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/user-spot-create-v1.json"
    ZLINK_USER_SPOT_CLOSE_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/user-spot-close-v1.json"
    ZLINK_ACTOR_CREATE_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/actor-create-v1.json"
    ZLINK_ZLJR_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/zljr-v1.json"
    ZLINK_ACTOR_JOIN_REQUEST_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/actor-join-request-v1.json"
    ZLINK_ACTOR_JOIN_REPLY_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/actor-join-reply-v1.json")
  target_link_libraries(test_cpp_framework_service_wire_codec PRIVATE
    zlink::cpp
    zlink::framework_provider_abstractions
    nlohmann_json::nlohmann_json
    opentelemetry-cpp::api
    Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_service_wire_codec)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_service_wire_codec
    COMMAND test_cpp_framework_service_wire_codec
    LABELS "framework-unit;framework-foundation;protocol"
    TIMEOUT 30)

  add_executable(test_cpp_framework_service_wire_codec_generated
    tests/Zlink.Framework.UnitTests/test_cpp_framework_service_wire_codec_generated.cpp)
  target_compile_features(test_cpp_framework_service_wire_codec_generated PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_service_wire_codec_generated PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_compile_definitions(test_cpp_framework_service_wire_codec_generated PRIVATE
    ZLINK_SERVICE_WIRE_FIXTURE_INDEX_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/fixtures/index.json")
  target_link_libraries(test_cpp_framework_service_wire_codec_generated PRIVATE
    nlohmann_json::nlohmann_json)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_service_wire_codec_generated)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_service_wire_codec_generated
    COMMAND test_cpp_framework_service_wire_codec_generated
    LABELS "framework-unit;protocol"
    TIMEOUT 30)

  add_executable(test_cpp_framework_endpoint_notation
    tests/Zlink.Framework.UnitTests/test_cpp_framework_endpoint_notation.cpp)
  target_compile_features(test_cpp_framework_endpoint_notation PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_endpoint_notation PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_endpoint_notation
    COMMAND test_cpp_framework_endpoint_notation
    LABELS "framework-unit;framework-foundation;protocol"
    TIMEOUT 30)

  add_executable(test_cpp_framework_authority_key_codec
    tests/Zlink.Framework.UnitTests/test_cpp_framework_authority_key_codec.cpp)
  target_compile_features(test_cpp_framework_authority_key_codec PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_authority_key_codec PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  target_compile_definitions(test_cpp_framework_authority_key_codec PRIVATE
    ZLINK_AUTHORITY_KEY_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/authority-key-v1.json")
  target_link_libraries(test_cpp_framework_authority_key_codec PRIVATE
    zlink::framework
    nlohmann_json::nlohmann_json)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_authority_key_codec
    COMMAND test_cpp_framework_authority_key_codec
    LABELS "framework-unit;framework-foundation;protocol"
    TIMEOUT 30)

  add_executable(test_cpp_framework_relocation_envelope_golden
    tests/Zlink.Framework.UnitTests/test_cpp_framework_relocation_envelope_golden.cpp)
  target_compile_features(test_cpp_framework_relocation_envelope_golden PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_relocation_envelope_golden PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_compile_definitions(test_cpp_framework_relocation_envelope_golden PRIVATE
    ZLINK_RELOCATION_ENVELOPE_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/relocation-envelope-v1.json")
  target_link_libraries(test_cpp_framework_relocation_envelope_golden PRIVATE
    nlohmann_json::nlohmann_json
    zlink::framework
    Threads::Threads)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_relocation_envelope_golden
    COMMAND test_cpp_framework_relocation_envelope_golden
    LABELS "framework-unit;framework-foundation;protocol"
    TIMEOUT 30)

  add_executable(test_cpp_framework_actor_authority_payload
    tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_authority_payload.cpp)
  target_compile_features(test_cpp_framework_actor_authority_payload PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_actor_authority_payload PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  target_compile_definitions(test_cpp_framework_actor_authority_payload PRIVATE
    ZLINK_DURABLE_AUTHORITY_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/durable-authority-v1.json"
    ZLINK_AUTHORITY_RELOCATION_STATE_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/authority-relocation-state-v1.json")
  target_link_libraries(test_cpp_framework_actor_authority_payload PRIVATE
    nlohmann_json::nlohmann_json
    zlink::framework)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_actor_authority_payload
    COMMAND test_cpp_framework_actor_authority_payload
    LABELS "framework-unit;framework-contract;protocol"
    TIER unit
    TIMEOUT 30)

  add_executable(test_cpp_framework_store_record_golden
    tests/Zlink.Framework.UnitTests/test_cpp_framework_store_record_golden.cpp)
  target_compile_features(test_cpp_framework_store_record_golden PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_store_record_golden PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_DIR}/extensions/framework-locations-redis/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/include)
  target_compile_definitions(test_cpp_framework_store_record_golden PRIVATE
    ZLINK_STORE_RECORD_GOLDEN_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/golden/store-record-v1.json")
  target_link_libraries(test_cpp_framework_store_record_golden PRIVATE
    nlohmann_json::nlohmann_json
    zlink::framework
    Threads::Threads)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_store_record_golden
    COMMAND test_cpp_framework_store_record_golden
    LABELS "framework-unit;framework-foundation;protocol"
    TIMEOUT 30)

  add_executable(test_cpp_framework_operation_registry
    tests/Zlink.Framework.UnitTests/test_cpp_framework_operation_registry.cpp
    framework/src/runtime/foundation/operation_registry.cpp)
  target_compile_features(test_cpp_framework_operation_registry PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_operation_registry PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  target_compile_definitions(test_cpp_framework_operation_registry PRIVATE
    ZLINK_COMPLETION_TERMINAL_CONFORMANCE_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/conformance/completion-terminal-v1.json")
  target_link_libraries(test_cpp_framework_operation_registry PRIVATE
    opentelemetry-cpp::metrics
    Threads::Threads
    nlohmann_json::nlohmann_json)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_operation_registry
    COMMAND test_cpp_framework_operation_registry
    LABELS "framework-unit;framework-foundation;operation;resource"
    TIMEOUT 30)

  add_executable(test_cpp_framework_m6a_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp)
  target_compile_features(test_cpp_framework_m6a_runtime PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_m6a_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_m6a_runtime PRIVATE
    opentelemetry-cpp::metrics
    zlink_framework_m6a_runtime
    zlink_framework
    Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_m6a_runtime)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_m6a_runtime
    COMMAND test_cpp_framework_m6a_runtime
    LABELS "framework-unit;framework-m6-runtime;topology;mailbox;liveness;raw-binding"
    TIMEOUT 30)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_owner_rejection_observability
    COMMAND test_cpp_framework_m6a_runtime
      --owner-rejection-observability
    LABELS "framework-unit"
    TIMEOUT 0)

  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_r6_create_lifecycle
    COMMAND test_cpp_framework_m6a_runtime
      --r6-create-lifecycle
    LABELS "framework-unit;framework-regression"
    TIMEOUT 30)

  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_r6_join_durable
    COMMAND test_cpp_framework_m6a_runtime
      --r6-join-durable
    LABELS "framework-unit;framework-regression"
    TIMEOUT 30)

  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_r6_join_owner_loss
    COMMAND test_cpp_framework_m6a_runtime
      --r6-join-owner-loss
    LABELS "framework-unit;framework-regression"
    TIMEOUT 30)

  add_executable(test_cpp_framework_mesh_shutdown_seal
    tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_shutdown_seal.cpp)
  target_compile_features(test_cpp_framework_mesh_shutdown_seal PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_mesh_shutdown_seal PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_mesh_shutdown_seal PRIVATE
    zlink_framework_m6a_runtime zlink_framework Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_mesh_shutdown_seal)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_mesh_shutdown_seal
    COMMAND test_cpp_framework_mesh_shutdown_seal
    LABELS "framework-unit;topology;liveness;raw-binding"
    TIMEOUT 30)

  add_executable(test_cpp_framework_m6b_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp)
  target_compile_features(test_cpp_framework_m6b_runtime PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_m6b_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_m6b_runtime PRIVATE
    opentelemetry-cpp::metrics
    zlink_framework_m6b_runtime
    zlink_framework
    Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_m6b_runtime)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_m6b_runtime
    COMMAND test_cpp_framework_m6b_runtime
    LABELS "framework-unit;framework-m6-runtime;stateful;spot;actor;stream-session"
    TIMEOUT 30)

  add_executable(test_cpp_framework_reply_submit_claim
    tests/Zlink.Framework.UnitTests/test_cpp_framework_reply_submit_claim.cpp)
  target_compile_features(test_cpp_framework_reply_submit_claim PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_reply_submit_claim PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_reply_submit_claim PRIVATE
    zlink_framework_m6b_runtime
    zlink_framework
    Threads::Threads)
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_reply_submit_claim)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_reply_submit_claim
    COMMAND test_cpp_framework_reply_submit_claim
    LABELS "framework-unit;framework-m6-runtime;stateful;reply;claim"
    TIMEOUT 10)

  add_executable(test_cpp_framework_m6c_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_m6c_runtime.cpp)
  target_compile_features(test_cpp_framework_m6c_runtime PRIVATE cxx_std_20)
  target_include_directories(test_cpp_framework_m6c_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_m6c_runtime PRIVATE
    zlink_framework
    Threads::Threads)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_m6c_runtime
    COMMAND test_cpp_framework_m6c_runtime
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
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_host_lifecycle
    COMMAND test_cpp_framework_host_lifecycle
    LABELS "framework-unit;framework-m6-runtime;maintenance;relocation;termination"
    TIMEOUT 30)

endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_TESTS)
  add_zlink_protobuf_schema(zlink_framework_bingo_proto
    ${ZLINK_FRAMEWORK_CPP_DIR}/samples/Bingo/Shared/Contracts/bingo_messages.proto)
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_TESTS)
  if(CMAKE_CONFIGURATION_TYPES)
    set(ZLINK_FRAMEWORK_CPP_TEST_CONFIGURATION "$<CONFIG>")
    set(ZLINK_FRAMEWORK_CPP_TEST_IS_MULTI_CONFIG ON)
  else()
    set(ZLINK_FRAMEWORK_CPP_TEST_CONFIGURATION "${CMAKE_BUILD_TYPE}")
    set(ZLINK_FRAMEWORK_CPP_TEST_IS_MULTI_CONFIG OFF)
  endif()

  add_executable(zlink_cpp_framework_mesh_node_vertical_test
    tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_node_vertical.cpp)
  zlink_framework_cpp_enable_test_assertions(
    zlink_cpp_framework_mesh_node_vertical_test)
  target_include_directories(zlink_cpp_framework_mesh_node_vertical_test PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp
    ${ZLINK_FRAMEWORK_CPP_DIR}/tests/support)
  target_link_libraries(zlink_cpp_framework_mesh_node_vertical_test PRIVATE
    zlink::framework
    zlink::stream_connector
    Threads::Threads)
  zlink_framework_cpp_add_boost_headers(
    zlink_cpp_framework_mesh_node_vertical_test)
  zlink_framework_cpp_add_test(
    NAME zlink_cpp_framework_mesh_node_vertical_test
    COMMAND zlink_cpp_framework_mesh_node_vertical_test
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

  # For targets whose TEST macros supply every case and that define no main of
  # their own. See tests/support/zlink_test_main.cpp for why a packaged
  # gtest_main or gmock_main cannot be borrowed here.
  function(target_use_zlink_gtest_main target_name)
    target_sources(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/tests/support/zlink_test_main.cpp)
  endfunction()

  function(add_zlink_framework_test target_name source_file labels)
    set(tier "${ARGV3}")
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE zlink::framework)
    target_link_zlink_test_harness(${target_name})
    target_include_directories(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
      ${ZLINK_FRAMEWORK_CPP_DIR}/tests/support)
    set(test_tier_args)
    if(tier)
      list(APPEND test_tier_args TIER "${tier}")
    endif()
    zlink_framework_cpp_add_test(
      NAME ${target_name}
      COMMAND ${target_name}
      LABELS "${labels}"
      ${test_tier_args}
      TIMEOUT 30)
  endfunction()

  function(add_zlink_connector_test target_name source_file labels)
    set(tier "${ARGV3}")
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE zlink::stream_connector)
    target_link_zlink_test_harness(${target_name})
    target_include_directories(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/src)
    # Test sources include Boost directly; they must resolve the same Boost
    # tree as the connector objects (see zlink_framework_cpp_add_boost_headers).
    zlink_framework_cpp_add_boost_headers(${target_name})
    set(test_tier_args)
    if(tier)
      list(APPEND test_tier_args TIER "${tier}")
    endif()
    zlink_framework_cpp_add_test(
      NAME ${target_name}
      COMMAND ${target_name}
      LABELS "${labels}"
      ${test_tier_args}
      TIMEOUT 30)
  endfunction()

  function(add_zlink_http_client_test target_name source_file labels)
    set(tier "${ARGV3}")
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE zlink::http_client)
    target_link_zlink_test_harness(${target_name})
    target_include_directories(${target_name} PRIVATE
      ${ZLINK_FRAMEWORK_CPP_DIR}/http-client/src)
    # Test sources include Boost (beast test server); they must resolve the
    # same Boost tree as the http client objects.
    zlink_framework_cpp_add_boost_headers(${target_name})
    if(ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL)
      list(APPEND labels http-client-https)
    endif()
    set(test_tier_args)
    if(tier)
      list(APPEND test_tier_args TIER "${tier}")
    endif()
    zlink_framework_cpp_add_test(
      NAME ${target_name}
      COMMAND ${target_name}
      LABELS "${labels}"
      ${test_tier_args}
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
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_contract_headers
    COMMAND test_cpp_framework_contract_headers
    LABELS "framework-contract"
    TIMEOUT 30)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_async_contract
    COMMAND test_cpp_framework_contract_headers
    LABELS "framework-contract;framework-unit;async"
    TIER unit
    TIMEOUT 30)

  add_executable(test_cpp_framework_provider_task
    tests/Zlink.Framework.ContractTests/test_cpp_framework_provider_task.cpp)
  target_link_libraries(test_cpp_framework_provider_task PRIVATE
    zlink::framework_provider_abstractions Threads::Threads)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_provider_task
    COMMAND test_cpp_framework_provider_task
    LABELS "framework-contract;async"
    TIMEOUT 30)

  add_executable(test_cpp_framework_sample_parity
    tests/Zlink.Framework.ContractTests/test_cpp_framework_sample_parity.cpp)
  target_link_libraries(test_cpp_framework_sample_parity PRIVATE
    zlink::framework
    zlink::framework_codec_protobuf
    zlink::stream_connector
    zlink_framework_bingo_proto
    GTest::gtest
    GTest::gmock)
  target_include_directories(test_cpp_framework_sample_parity PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_sample_parity
    COMMAND test_cpp_framework_sample_parity
    LABELS "framework-contract;framework-sample-parity"
    TIMEOUT 30)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_parity_contract
    COMMAND test_cpp_framework_sample_parity
    LABELS "framework-regression;framework-sample-parity;parity"
    TIER integration
    TIMEOUT 30)

  add_executable(test_cpp_framework_layout_contract
    tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp)
  target_compile_definitions(test_cpp_framework_layout_contract PRIVATE
    ZLINK_FRAMEWORK_CPP_SOURCE_DIR="${ZLINK_FRAMEWORK_CPP_DIR}")
  target_compile_features(test_cpp_framework_layout_contract PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_layout_contract)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_layout_contract
    COMMAND test_cpp_framework_layout_contract
    LABELS "framework-contract"
    TIMEOUT 30)

  add_executable(test_cpp_framework_target_contract
    tests/Zlink.Framework.ContractTests/test_cpp_framework_target_contract.cpp)
  target_compile_definitions(test_cpp_framework_target_contract PRIVATE
    ZLINK_FRAMEWORK_CPP_SOURCE_DIR="${ZLINK_FRAMEWORK_CPP_DIR}")
  target_compile_features(test_cpp_framework_target_contract PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_target_contract)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_target_contract
    COMMAND test_cpp_framework_target_contract
    LABELS "framework-contract"
    TIMEOUT 60)
  set(ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS
    -D ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
    -D ZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR})
  if(WIN32)
    list(APPEND ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS
      -D ZLINK_FRAMEWORK_CPP_TOOLING_GENERATOR=${CMAKE_GENERATOR}
      -D ZLINK_FRAMEWORK_CPP_TOOLING_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
      -D ZLINK_FRAMEWORK_CPP_TOOLING_VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}
      -D ZLINK_FRAMEWORK_CPP_TOOLING_VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET})
  endif()
  if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS
      -D ZLINK_FRAMEWORK_CPP_TOOLING_CMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
  endif()
  foreach(tooling_dependency IN ITEMS
      protobuf_DIR
      absl_DIR
      utf8_range_DIR
      ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX
      ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX)
    if(DEFINED ${tooling_dependency} AND NOT "${${tooling_dependency}}" STREQUAL "")
      list(APPEND ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS
        -D ZLINK_FRAMEWORK_CPP_TOOLING_${tooling_dependency}=${${tooling_dependency}})
    endif()
  endforeach()
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_tooling_contract
    COMMAND ${CMAKE_COMMAND}
      ${ZLINK_FRAMEWORK_CPP_TOOLING_CONTRACT_ARGS}
      -P
      ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.ContractTests/verify_tooling_contract.cmake
    LABELS "framework-contract;framework-package;framework-tooling"
    TIMEOUT 120
    PROPERTIES
      RESOURCE_LOCK "zlink-cpp-vcpkg")
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_label_contract
    COMMAND ${CMAKE_COMMAND}
      -D
      ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
      -D
      ZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -D
      ZLINK_FRAMEWORK_CPP_CONFIGURATION=${ZLINK_FRAMEWORK_CPP_TEST_CONFIGURATION}
      -D
      ZLINK_FRAMEWORK_CPP_EXPECT_COVERAGE_LABEL=${ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE}
      -P
      ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.ContractTests/verify_ctest_label_contract.cmake
    LABELS "framework-contract;framework-regression"
    TIMEOUT 30)

  add_zlink_framework_test(
    test_cpp_framework_deliverydispatch_readiness
    tests/Zlink.Framework.UnitTests/test_cpp_framework_deliverydispatch_readiness.cpp
    "framework-unit;framework-regression")
  add_zlink_framework_test(
    test_cpp_framework_app_host
    tests/Zlink.Framework.UnitTests/test_cpp_framework_app_host.cpp
    "framework-unit;framework-host;framework-config;framework-http;framework-http-e2e")
  add_zlink_framework_test(
    test_cpp_framework_http_startup
    tests/Zlink.Framework.UnitTests/test_cpp_framework_http_startup.cpp
    "framework-unit;framework-contract;framework-host;framework-http;framework-regression"
    unit)
  zlink_framework_cpp_add_boost_headers(test_cpp_framework_http_startup)
  target_link_libraries(test_cpp_framework_http_startup PRIVATE GTest::gtest_main)
  add_zlink_framework_test(
    test_cpp_framework_listener_identity
    tests/Zlink.Framework.UnitTests/test_cpp_framework_listener_identity.cpp
    "framework-unit;framework-contract;framework-regression;framework-zlink-channel"
    unit)
  target_link_libraries(test_cpp_framework_listener_identity PRIVATE GTest::gtest_main)
  target_include_directories(test_cpp_framework_listener_identity PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_client_server_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_client_server_runtime.cpp
    "framework-unit;framework-regression;framework-client-server;framework-monitoring")
  target_include_directories(test_cpp_framework_client_server_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  set_tests_properties(test_cpp_framework_client_server_runtime PROPERTIES
    RESOURCE_LOCK "zlink_cpp_heavy_runtime")
  add_zlink_framework_test(
    test_cpp_framework_actor_gateway
    tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_gateway.cpp
    "framework-unit;framework-regression;framework-actor;stream")
  target_link_libraries(test_cpp_framework_actor_gateway PRIVATE
    zlink::framework_codec_protobuf)
  target_include_directories(test_cpp_framework_actor_gateway PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  foreach(reconcile_case source no-commit target indeterminate)
    zlink_framework_cpp_add_test(
      NAME test_cpp_framework_reconcile_${reconcile_case}
      COMMAND test_cpp_framework_actor_gateway
        --reconcile-${reconcile_case}
      LABELS "framework-unit;framework-regression;framework-actor;relocation"
      TIMEOUT 30)
  endforeach()
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
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_http_integration
    COMMAND test_cpp_framework_app_host
    LABELS "framework-integration;framework-http;framework-http-e2e;http"
    TIMEOUT 30)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_http_sync_submit
    COMMAND test_cpp_framework_app_host
      --http-submit-only
    LABELS "framework-unit;framework-regression;framework-http"
    TIMEOUT 30)
  if(ZLINK_FRAMEWORK_CPP_REQUIRE_HTTP_PERF_REPORT)
    zlink_framework_cpp_add_test(
      NAME test_cpp_framework_http_perf_gate
      COMMAND ${CMAKE_COMMAND}
        -D
        ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
        -P
        ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.PerfTests/http_perf_gate.cmake
      LABELS "framework-http-perf;framework-regression;http"
      TIMEOUT 30
      ENVIRONMENT "ZLINK_FRAMEWORK_HTTP_PERF_REQUIRED=1")
  endif()
  add_executable(test_cpp_framework_entry_spot_admission_burst
    tests/Zlink.Framework.PerfTests/entry_spot_admission_burst.cpp)
  target_include_directories(test_cpp_framework_entry_spot_admission_burst PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_entry_spot_admission_burst PRIVATE
    zlink_framework Threads::Threads)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_entry_spot_admission_burst
    COMMAND test_cpp_framework_entry_spot_admission_burst
    LABELS "framework-perf-smoke;framework-regression;spot-actor"
    TIER integration
    TIMEOUT 30)
  set_tests_properties(
    test_cpp_framework_app_host
    test_cpp_framework_http_integration
    test_cpp_framework_http_sync_submit
    PROPERTIES
      RESOURCE_LOCK zlink_cpp_framework_http_ports)
  add_zlink_framework_test(
    test_cpp_framework_DI_scope
    tests/Zlink.Framework.UnitTests/test_cpp_framework_di.cpp
    "framework-unit;framework-regression;DI;scope")
  add_zlink_framework_test(
    test_cpp_framework_runtime_integration
    tests/Zlink.Framework.UnitTests/test_cpp_framework_runtime_integration.cpp
    "framework-unit;framework-integration;runtime"
    unit)
  add_zlink_framework_test(
    test_cpp_framework_async_task
    tests/Zlink.Framework.UnitTests/test_cpp_framework_async_task.cpp
    "framework-unit;framework-regression;async")
  add_zlink_framework_test(
    test_cpp_framework_executor_owner
    tests/Zlink.Framework.UnitTests/test_cpp_framework_executor_owner.cpp
    "framework-unit;framework-regression;framework-host;async")
  add_zlink_framework_test(
    test_cpp_framework_actor_join_handoff_id
    tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_join_handoff_id.cpp
    "framework-unit;framework-regression;actor-join")
  target_include_directories(test_cpp_framework_actor_join_handoff_id PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_execution
    tests/Zlink.Framework.UnitTests/test_cpp_framework_execution.cpp
    "framework-unit;framework-contract;framework-regression;execution;spot;yield"
    unit)
  target_include_directories(test_cpp_framework_execution PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_compile_definitions(test_cpp_framework_execution PRIVATE
    ZLINK_SERIAL_EXECUTION_CONFORMANCE_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/conformance/serial-execution-v1.json"
    ZLINK_RUNTIME_OBSERVATION_CONFORMANCE_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/conformance/runtime-observation-v1.json")
  add_executable(test_cpp_framework_state_lane
    tests/Zlink.Framework.UnitTests/test_cpp_framework_state_lane.cpp)
  target_link_libraries(test_cpp_framework_state_lane PRIVATE
    zlink::framework
    GTest::gtest
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_state_lane PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_enable_coverage(test_cpp_framework_state_lane)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_state_lane
    COMMAND test_cpp_framework_state_lane
    LABELS "framework-unit;framework-regression;execution"
    TIMEOUT 30)
  add_executable(test_cpp_framework_lane_roundtrip
    tests/Zlink.Framework.UnitTests/test_cpp_framework_lane_roundtrip.cpp)
  target_link_libraries(test_cpp_framework_lane_roundtrip PRIVATE
    zlink::framework
    GTest::gtest
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_lane_roundtrip PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_enable_coverage(test_cpp_framework_lane_roundtrip)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_lane_roundtrip
    COMMAND test_cpp_framework_lane_roundtrip
    LABELS "framework-unit;framework-regression;execution"
    TIMEOUT 30)
  add_zlink_framework_test(
    test_cpp_framework_sync_submit
    tests/Zlink.Framework.UnitTests/test_cpp_framework_sync_submit.cpp
    "framework-unit;framework-regression;execution;messaging")
  target_link_libraries(test_cpp_framework_sync_submit PRIVATE GTest::gtest_main)
  target_include_directories(test_cpp_framework_sync_submit PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_shared_serial_turn
    tests/Zlink.Framework.UnitTests/test_cpp_framework_shared_serial_turn.cpp
    "framework-unit;framework-regression;execution;actor-join")
  target_link_libraries(test_cpp_framework_shared_serial_turn PRIVATE GTest::gtest_main)
  add_zlink_framework_test(
    test_cpp_framework_serializer_registry
    tests/Zlink.Framework.UnitTests/test_cpp_framework_serializer_registry.cpp
    "framework-unit;serializer")
  target_compile_definitions(test_cpp_framework_serializer_registry PRIVATE
    ZLINK_CODEC_SELECTION_CONFORMANCE_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/conformance/codec-selection-v1.json"
    ZLINK_PAYLOAD_OWNERSHIP_CONFORMANCE_PATH="${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/conformance/payload-ownership-v1.json")
  add_executable(test_cpp_framework_location_key_codec
    tests/Zlink.Framework.UnitTests/test_cpp_framework_location_key_codec.cpp)
  target_compile_features(test_cpp_framework_location_key_codec PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_location_key_codec)
  target_use_zlink_gtest_main(test_cpp_framework_location_key_codec)
  target_link_libraries(test_cpp_framework_location_key_codec PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_location_key_codec PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_location_key_codec
    COMMAND test_cpp_framework_location_key_codec
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)
  add_executable(test_cpp_framework_in_memory_location_store
    tests/Zlink.Framework.UnitTests/test_cpp_framework_in_memory_location_store.cpp)
  target_compile_features(test_cpp_framework_in_memory_location_store PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_in_memory_location_store)
  target_use_zlink_gtest_main(test_cpp_framework_in_memory_location_store)
  target_link_libraries(test_cpp_framework_in_memory_location_store PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_in_memory_location_store PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_in_memory_location_store
    COMMAND test_cpp_framework_in_memory_location_store
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)

  add_executable(test_cpp_framework_opaque_store_providers
    tests/Zlink.Framework.UnitTests/test_cpp_framework_opaque_store_providers.cpp)
  target_compile_features(test_cpp_framework_opaque_store_providers PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_opaque_store_providers)
  target_use_zlink_gtest_main(test_cpp_framework_opaque_store_providers)
  target_link_libraries(test_cpp_framework_opaque_store_providers PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_opaque_store_providers PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  # The bounded-batch aggregate stress case commits 2050 participants through
  # the in-memory fixture store and legitimately runs for tens of seconds.
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_opaque_store_providers
    COMMAND test_cpp_framework_opaque_store_providers
    LABELS "framework-unit;framework-location;framework-regression"
    TIMEOUT 120)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_r6_creation_terminal
    COMMAND test_cpp_framework_opaque_store_providers
      --gtest_filter=*CreationTerminal*
    LABELS "framework-unit;framework-regression"
    TIMEOUT 30)

  add_executable(test_cpp_framework_location_runtime
    tests/Zlink.Framework.UnitTests/test_cpp_framework_location_runtime.cpp)
  target_compile_features(test_cpp_framework_location_runtime PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_location_runtime)
  target_use_zlink_gtest_main(test_cpp_framework_location_runtime)
  target_link_libraries(test_cpp_framework_location_runtime PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_location_runtime PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_location_runtime
    COMMAND test_cpp_framework_location_runtime
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)
  add_executable(test_cpp_framework_location_lifecycle
    tests/Zlink.Framework.UnitTests/test_cpp_framework_location_lifecycle.cpp)
  target_compile_features(test_cpp_framework_location_lifecycle PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_location_lifecycle)
  target_use_zlink_gtest_main(test_cpp_framework_location_lifecycle)
  target_link_libraries(test_cpp_framework_location_lifecycle PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_location_lifecycle PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_location_lifecycle
    COMMAND test_cpp_framework_location_lifecycle
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30)
  add_executable(test_cpp_framework_store_location_resolvers
    tests/Zlink.Framework.UnitTests/test_cpp_framework_store_location_resolvers.cpp)
  target_compile_features(test_cpp_framework_store_location_resolvers PRIVATE cxx_std_20)
  target_link_zlink_test_harness(test_cpp_framework_store_location_resolvers)
  target_use_zlink_gtest_main(test_cpp_framework_store_location_resolvers)
  target_link_libraries(test_cpp_framework_store_location_resolvers PRIVATE
    zlink::framework
    GTest::gtest_main)
  target_include_directories(test_cpp_framework_store_location_resolvers PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/include
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  zlink_framework_cpp_enable_test_assertions(
    test_cpp_framework_store_location_resolvers)
  zlink_framework_cpp_add_boost_headers(test_cpp_framework_store_location_resolvers)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_store_location_resolvers
    COMMAND test_cpp_framework_store_location_resolvers
    LABELS "framework-unit;framework-regression;framework-location"
    TIMEOUT 30
    PROPERTIES
      RESOURCE_LOCK zlink_cpp_heavy_runtime)
  add_zlink_framework_test(
    test_cpp_framework_handler_registry
    tests/Zlink.Framework.UnitTests/test_cpp_framework_handler_registry.cpp
    "framework-unit;handler")
  add_zlink_framework_test(
    test_cpp_framework_channel_messaging
    tests/Zlink.Framework.UnitTests/test_cpp_framework_channel_messaging.cpp
    "framework-unit;framework-regression;channel")
  add_zlink_framework_test(
    test_cpp_framework_channel_no_eligible_member
    tests/Zlink.Framework.UnitTests/test_cpp_framework_channel_no_eligible_member.cpp
    "framework-unit;framework-regression;channel;messaging")
  target_use_zlink_gtest_main(test_cpp_framework_channel_no_eligible_member)
  target_include_directories(test_cpp_framework_channel_no_eligible_member PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_fanout_subscription
    tests/Zlink.Framework.UnitTests/test_cpp_framework_fanout_subscription.cpp
    "framework-unit;framework-regression;channel;messaging")
  target_use_zlink_gtest_main(test_cpp_framework_fanout_subscription)
  target_include_directories(test_cpp_framework_fanout_subscription PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  set_tests_properties(test_cpp_framework_fanout_subscription PROPERTIES
    RESOURCE_LOCK zlink_cpp_heavy_runtime)
  zlink_framework_cpp_add_boost_headers(test_cpp_framework_channel_messaging)
  target_link_libraries(test_cpp_framework_channel_messaging PRIVATE
    zlink::framework_codec_protobuf)
  add_zlink_framework_test(
    test_cpp_framework_fanout_admission
    tests/Zlink.Framework.UnitTests/test_cpp_framework_fanout_admission.cpp
    "framework-unit;framework-regression;channel;admission")
  target_link_libraries(test_cpp_framework_fanout_admission PRIVATE
    GTest::gtest_main)
  add_zlink_framework_test(
    test_cpp_framework_fanout_no_drop
    tests/Zlink.Framework.UnitTests/test_cpp_framework_fanout_no_drop.cpp
    "framework-unit;framework-regression;channel;backpressure")
  target_link_libraries(test_cpp_framework_fanout_no_drop PRIVATE
    GTest::gtest_main)
  add_zlink_framework_test(
    test_cpp_framework_channel_reply_admission
    tests/Zlink.Framework.UnitTests/test_cpp_framework_channel_reply_admission.cpp
    "framework-unit;framework-regression;channel;reply;admission")
  target_compile_definitions(test_cpp_framework_channel_reply_admission PRIVATE
    ZLINK_FRAMEWORK_CPP_SOURCE_DIR="${ZLINK_FRAMEWORK_CPP_DIR}")
  target_include_directories(test_cpp_framework_channel_reply_admission PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_channel_reply_admission PRIVATE
    GTest::gtest_main)
  add_zlink_framework_test(
    test_cpp_framework_route_mesh_weight_update
    tests/Zlink.Framework.UnitTests/test_cpp_framework_route_mesh_weight_update.cpp
    "framework-unit;framework-regression;channel;messaging")
  target_use_zlink_gtest_main(test_cpp_framework_route_mesh_weight_update)
  target_include_directories(test_cpp_framework_route_mesh_weight_update PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_application_job_queue
    tests/Zlink.Framework.UnitTests/test_cpp_framework_application_job_queue.cpp
    "framework-unit;framework-regression;execution;admission")
  target_link_libraries(test_cpp_framework_application_job_queue PRIVATE
    GTest::gtest_main)
  add_zlink_framework_test(
    test_cpp_framework_instance_spot_activation
    tests/Zlink.Framework.UnitTests/test_cpp_framework_instance_spot_activation.cpp
    "framework-unit;framework-regression;spot;instance-activation")
  target_include_directories(test_cpp_framework_instance_spot_activation PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  target_link_libraries(test_cpp_framework_instance_spot_activation PRIVATE
    GTest::gtest_main)
  # The channel messaging test reaches the service wire codec headers, which include the
  # schema-generated constants header (see test_cpp_framework_service_wire_codec above).
  target_include_directories(test_cpp_framework_channel_messaging PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
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
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  if(OpenSSL_FOUND AND ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL)
    target_compile_definitions(test_cpp_framework_stream_framework PRIVATE
      ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
      ZLINK_FRAMEWORK_STREAM_TEST_CERT="${ZLINK_HTTP_CLIENT_TEST_CERT}"
      ZLINK_FRAMEWORK_STREAM_TEST_KEY="${ZLINK_HTTP_CLIENT_TEST_KEY}")
    target_link_libraries(test_cpp_framework_stream_framework PRIVATE
      OpenSSL::SSL OpenSSL::Crypto)
  endif()
  add_zlink_framework_test(
    test_cpp_framework_poll_interval_wait
    tests/Zlink.Framework.UnitTests/test_cpp_framework_poll_interval_wait.cpp
    "framework-unit;framework-regression")
  add_zlink_framework_test(
    test_cpp_framework_message_flow
    tests/Zlink.Framework.UnitTests/test_cpp_framework_message_flow.cpp
    "framework-unit;framework-regression;framework-observability;diagnostics")
  add_zlink_framework_test(
    test_cpp_framework_monitoring
    tests/Zlink.Framework.UnitTests/test_cpp_framework_monitoring.cpp
    "framework-unit;framework-regression;framework-observability;metrics")
  target_link_libraries(test_cpp_framework_monitoring PRIVATE opentelemetry-cpp::metrics)
  add_zlink_framework_test(
    test_cpp_framework_mesh_metrics
    tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_metrics.cpp
    "framework-unit;framework-regression;framework-observability;metrics")
  target_link_libraries(test_cpp_framework_mesh_metrics PRIVATE
    opentelemetry-cpp::metrics)
  target_include_directories(test_cpp_framework_mesh_metrics PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  add_zlink_framework_test(
    test_cpp_framework_messaging
    tests/Zlink.Framework.UnitTests/test_cpp_framework_messaging.cpp
    "framework-unit;framework-regression;messaging")
  zlink_framework_cpp_enable_test_assertions(test_cpp_framework_messaging)
  target_include_directories(test_cpp_framework_messaging PRIVATE
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
  add_executable(test_cpp_framework_gtest_harness
    tests/Zlink.Framework.UnitTests/test_cpp_framework_gtest_harness.cpp)
  target_link_libraries(test_cpp_framework_gtest_harness PRIVATE
    zlink::framework
    GTest::gtest_main
    GTest::gmock)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_gtest_harness
    COMMAND test_cpp_framework_gtest_harness
    LABELS "framework-unit;framework-regression;gtest"
    TIMEOUT 30)
  add_executable(test_cpp_framework_redis_blob_bound
    tests/Zlink.Framework.UnitTests/test_cpp_framework_redis_blob_bound.cpp
    extensions/framework-locations-redis/src/redis.cpp)
  zlink_enable_coverage(test_cpp_framework_redis_blob_bound)
  target_compile_features(test_cpp_framework_redis_blob_bound PRIVATE cxx_std_20)
  target_compile_definitions(test_cpp_framework_redis_blob_bound PRIVATE
    ZLINK_FRAMEWORK_LOCATIONS_REDIS_HAS_ASYNC_CLIENT=1)
  target_include_directories(test_cpp_framework_redis_blob_bound BEFORE PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/tests/support/fake_redis
    ${ZLINK_FRAMEWORK_CPP_DIR}/extensions/framework-locations-redis/include)
  target_link_libraries(test_cpp_framework_redis_blob_bound PRIVATE
    zlink::framework
    GTest::gtest_main)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_redis_blob_bound
    COMMAND test_cpp_framework_redis_blob_bound
    LABELS "framework-unit;framework-regression;framework-location;redis"
    TIMEOUT 30)
  if(TARGET redis++::redis++ OR TARGET redis++::redis++_static)
    add_executable(test_cpp_framework_locations_redis
      tests/Zlink.Framework.UnitTests/test_cpp_framework_locations_redis.cpp)
    zlink_enable_coverage(test_cpp_framework_locations_redis)
    target_link_libraries(test_cpp_framework_locations_redis PRIVATE
      zlink::framework_locations_redis
      GTest::gtest_main)
    if(TARGET redis++::redis++)
      target_link_libraries(test_cpp_framework_locations_redis PRIVATE redis++::redis++)
    else()
      target_link_libraries(test_cpp_framework_locations_redis PRIVATE redis++::redis++_static)
    endif()
    zlink_framework_cpp_add_test(
      NAME test_cpp_framework_locations_redis
      COMMAND test_cpp_framework_locations_redis
      LABELS "framework-extension;framework-location;redis"
      TIER integration
      TIMEOUT 30)
  endif()
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_install_consumer
    COMMAND ${CMAKE_COMMAND}
      -DZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DZLINK_FRAMEWORK_CPP_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-consumer-install
      -DZLINK_FRAMEWORK_CPP_CONFIGURATION=${ZLINK_FRAMEWORK_CPP_TEST_CONFIGURATION}
      -DZLINK_FRAMEWORK_CPP_IS_MULTI_CONFIG=${ZLINK_FRAMEWORK_CPP_TEST_IS_MULTI_CONFIG}
      "-DZLINK_FRAMEWORK_CPP_DEPENDENCY_PREFIX_PATH=${ZLINK_FRAMEWORK_CPP_PACKAGE_DEPENDENCY_PREFIX_PATH}"
      -DZLINK_FRAMEWORK_CPP_EXPECT_REDIS_PLUS_PLUS=${ZLINK_FRAMEWORK_REDIS_EXPORTS_REDIS_PLUS_PLUS}
      -DZLINK_FRAMEWORK_CPP_EXPECT_LIBUV=${ZLINK_FRAMEWORK_REDIS_EXPORTS_LIBUV}
      -P
      ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.PackageTests/install_consumer.cmake
    LABELS "framework-package;framework-regression;connector-package"
    TIER integration
    TIMEOUT 120)
  set(ZLINK_FRAMEWORK_CPP_STREAM_PACKAGE_EXPECTED_PATH_ARGS
    -DZLINK_FRAMEWORK_CPP_UNREAL_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:zlink_unreal_stream_connector>
    -DZLINK_FRAMEWORK_CPP_STREAM_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:zlink_stream_connector>
    -DZLINK_FRAMEWORK_CPP_BINDING_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:zlink::cpp>
    -DZLINK_FRAMEWORK_CPP_FRAMEWORK_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:zlink_framework>
    -DZLINK_FRAMEWORK_CPP_HTTP_CLIENT_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:zlink_http_client>)
  if(WIN32)
    list(APPEND ZLINK_FRAMEWORK_CPP_STREAM_PACKAGE_EXPECTED_PATH_ARGS
      -DZLINK_FRAMEWORK_CPP_CORE_RUNTIME_PATH=${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:libzlink>
      -DZLINK_FRAMEWORK_CPP_CORE_LINK_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}/$<TARGET_LINKER_FILE_NAME:libzlink>)
  else()
    list(APPEND ZLINK_FRAMEWORK_CPP_STREAM_PACKAGE_EXPECTED_PATH_ARGS
      -DZLINK_FRAMEWORK_CPP_CORE_RUNTIME_PATH=${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:libzlink>)
  endif()
  zlink_framework_cpp_add_test(
    NAME test_cpp_stream_connector_install_consumer
    COMMAND ${CMAKE_COMMAND}
      -DZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DZLINK_FRAMEWORK_CPP_CONFIGURATION=${ZLINK_FRAMEWORK_CPP_TEST_CONFIGURATION}
      -DZLINK_FRAMEWORK_CPP_IS_MULTI_CONFIG=${ZLINK_FRAMEWORK_CPP_TEST_IS_MULTI_CONFIG}
      ${ZLINK_FRAMEWORK_CPP_STREAM_PACKAGE_EXPECTED_PATH_ARGS}
      "-DZLINK_FRAMEWORK_CPP_DEPENDENCY_PREFIX_PATH=${ZLINK_FRAMEWORK_CPP_PACKAGE_DEPENDENCY_PREFIX_PATH}"
      -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=${ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX}
      -P
      ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.PackageTests/stream_connector_consumer.cmake
    LABELS "connector-package;connector-regression"
    TIER integration
    TIMEOUT 120
    PROPERTIES
      RESOURCE_LOCK zlink_cpp_package)
  add_zlink_connector_test(
    test_cpp_stream_connector
    tests/Systems.Zlink.Stream.Connector.Tests/test_cpp_stream_connector.cpp
    "connector-unit;connector-integration;connector-e2e;connector-contract;connector-protocol;connector-transport;connector-typed;connector-coroutine;connector-dispatch;connector-timeout"
    contract)
  add_executable(test_cpp_stream_connector_no_exceptions
    tests/Systems.Zlink.Stream.Connector.Tests/test_cpp_stream_connector_no_exceptions.cpp)
  target_compile_features(test_cpp_stream_connector_no_exceptions PRIVATE cxx_std_20)
  target_link_libraries(test_cpp_stream_connector_no_exceptions PRIVATE
    zlink::stream_connector_codecs)
  if(MSVC)
    target_compile_options(test_cpp_stream_connector_no_exceptions PRIVATE /EHs-c- /D_HAS_EXCEPTIONS=0)
  else()
    target_compile_options(test_cpp_stream_connector_no_exceptions PRIVATE -fno-exceptions)
  endif()
  zlink_framework_cpp_add_test(
    NAME test_cpp_stream_connector_no_exceptions
    COMMAND test_cpp_stream_connector_no_exceptions
    LABELS "connector-unit;connector-contract"
    TIER unit
    TIMEOUT 30)
  if(WIN32)
    add_zlink_connector_test(
      test_cpp_stream_connector_process_exit
      tests/Systems.Zlink.Stream.Connector.Tests/test_cpp_stream_connector_process_exit.cpp
      "connector-unit;connector-regression"
      contract)
    target_link_libraries(test_cpp_stream_connector_process_exit PRIVATE ws2_32)
  endif()
  # These tests each create a high-thread-count local runtime.  Serialise the
  # pair so CTest does not exercise their process-level resource usage as if it
  # were a product concurrency contract.
  set_tests_properties(test_cpp_stream_connector PROPERTIES
    RESOURCE_LOCK zlink_cpp_heavy_runtime)
  target_link_libraries(test_cpp_stream_connector PRIVATE
    zlink::cpp
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
  if(ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES)
    add_executable(sample_stream_connector_core_client
      connector/core/samples/core_client_smoke.cpp)
    target_link_libraries(sample_stream_connector_core_client PRIVATE
      zlink::stream_connector)
    zlink_framework_cpp_add_test(
      NAME sample_stream_connector_core_client
      COMMAND sample_stream_connector_core_client
      LABELS "connector-e2e;connector-contract"
      TIER e2e
      TIMEOUT 30
      PROPERTIES
        RESOURCE_LOCK zlink_cpp_heavy_runtime)

    add_executable(sample_stream_e2e_client
      connector/e2e-client/samples/e2e_client_smoke.cpp)
    target_link_libraries(sample_stream_e2e_client PRIVATE
      zlink::stream_e2e_client)
    zlink_framework_cpp_add_test(
      NAME sample_stream_e2e_client
      COMMAND sample_stream_e2e_client
      LABELS "connector-e2e"
      TIMEOUT 30
      PROPERTIES
        RESOURCE_LOCK zlink_cpp_heavy_runtime)

  endif()

  add_executable(connector_perf_client
    connector/perf/connector_perf_client.cpp)
  target_include_directories(connector_perf_client PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/src)
  zlink_framework_cpp_add_boost_headers(connector_perf_client)
  target_link_libraries(connector_perf_client PRIVATE
    zlink::stream_e2e_client)
  add_custom_target(connector_perf_smoke
    DEPENDS connector_perf_client)
  zlink_framework_cpp_add_test(
    NAME connector_perf_smoke
    COMMAND connector_perf_client
      --clients
      100
      --workers
      2
      --duration
      5s
      --warmup
      0s
      --request-timeout-ms
      100
      --loopback
      --report
      ${CMAKE_CURRENT_BINARY_DIR}/connector-perf-smoke.json
    LABELS "connector-perf-smoke;connector-perf-scale"
    TIER integration
    TIMEOUT 30)

  add_zlink_http_client_test(
    test_cpp_http_client
    http-client/tests/test_cpp_http_client.cpp
    "http-client-contract;http-client-unit;http-client-e2e;http-client-regression"
    integration)
  if(ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL)
    target_compile_definitions(test_cpp_http_client PRIVATE
      ZLINK_HTTP_CLIENT_TEST_WITH_OPENSSL
      ZLINK_HTTP_CLIENT_TEST_CERT="${ZLINK_HTTP_CLIENT_TEST_CERT}"
      ZLINK_HTTP_CLIENT_TEST_KEY="${ZLINK_HTTP_CLIENT_TEST_KEY}")
    target_link_libraries(test_cpp_http_client PRIVATE
      OpenSSL::SSL
      OpenSSL::Crypto)
  endif()

  add_executable(test_unreal_stream_connector
    tests/Zlink.Unreal.Stream.Connector.Tests/test_unreal_stream_connector.cpp)
  target_link_libraries(test_unreal_stream_connector PRIVATE
    zlink_unreal_stream_connector
    GTest::gtest
    GTest::gmock)
  zlink_framework_cpp_add_test(
    NAME test_unreal_stream_connector
    COMMAND test_unreal_stream_connector
    LABELS "connector-unreal-contract;connector-unreal-compile;connector-unreal-smoke"
    TIER integration
    TIMEOUT 30
    PROPERTIES
      RESOURCE_LOCK zlink_cpp_heavy_runtime)

  add_zlink_connector_test(
    test_unreal_adapter_push
    connector/engines/unreal/tests/test_unreal_stream_connector.cpp
    "connector-unreal-contract;connector-unreal-smoke"
    integration)
  target_link_libraries(test_unreal_adapter_push PRIVATE
    zlink_unreal_stream_connector
    zlink::cpp)

  if(ZLINK_STREAM_CONNECTOR_BUILD_GODOT)
    add_subdirectory(connector/engines/godot/tests)
  endif()
  if(ZLINK_STREAM_CONNECTOR_BUILD_AXMOL)
    add_subdirectory(connector/engines/axmol/tests)
  endif()

  set(ZLINK_UNREAL_PACKAGE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
  if(NOT ZLINK_UNREAL_PACKAGE_CONFIGURATION)
    set(ZLINK_UNREAL_PACKAGE_CONFIGURATION Release)
  endif()
  zlink_framework_cpp_add_test(
    NAME test_unreal_stream_connector_package_contract
    COMMAND ${CMAKE_COMMAND}
      -DZLINK_UNREAL_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DZLINK_UNREAL_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/unreal-package-contract
      -DZLINK_UNREAL_CONFIGURATION=${ZLINK_UNREAL_PACKAGE_CONFIGURATION}
      -P
      ${CMAKE_CURRENT_LIST_DIR}/connector/engines/unreal/Tools/package-third-party.cmake
    LABELS "connector-unreal-contract;connector-package"
    TIER integration
    TIMEOUT 120
    PROPERTIES
      RESOURCE_LOCK zlink_cpp_package)
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE)
  # Cross-language host (G6): the same role modes the peer languages' test
  # hosts expose, so every producer/consumer direction runs against the other
  # language's real package. It is part of the normal framework build; the
  # per-language scenario e2e apps above are opt-in.
  add_executable(zlink_cpp_cross_language_host
    cross-language/cross_language_host.cpp)
  target_link_libraries(zlink_cpp_cross_language_host PRIVATE
    zlink::framework
    zlink::framework_locations_redis
    zlink::stream_e2e_client)
  target_include_directories(zlink_cpp_cross_language_host PRIVATE
    ${ZLINK_FRAMEWORK_CPP_DIR}/framework/src
    ${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/generated/cpp)
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES)
  set(ZLINK_FRAMEWORK_CPP_SAMPLE_NAMES
    Bingo TicTacToe DeliveryDispatch GameQuest ShoppingMall SupportChat ZoneWorld)
  foreach(sample_name IN LISTS ZLINK_FRAMEWORK_CPP_SAMPLE_NAMES)
    # Group each sample's targets under samples/<Sample> in the IDE tree
    # instead of one flat "samples" list.
    set(CMAKE_FOLDER "samples/${sample_name}")
    add_subdirectory(samples/${sample_name})
  endforeach()
  unset(CMAKE_FOLDER)

  function(add_zlink_framework_sample_runner sample_name labels timeout_seconds)
    if(WIN32)
      set(sample_runner_command
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File
        "${ZLINK_FRAMEWORK_CPP_DIR}/samples/${sample_name}/run_sample.ps1")
    else()
      set(sample_runner_command
        bash "${ZLINK_FRAMEWORK_CPP_DIR}/samples/${sample_name}/run_sample.sh")
    endif()
    zlink_framework_cpp_add_test(
      NAME sample_smoke_sample_cpp_framework_${sample_name}
      COMMAND ${sample_runner_command}
      LABELS "${labels};framework-sample-smoke"
      TIER e2e
      TIMEOUT ${timeout_seconds}
      ENVIRONMENT "ZLINK_CPP_BUILD_DIR=${CMAKE_BINARY_DIR}"
      PROPERTIES
        RESOURCE_LOCK "zlink-cpp-sample-runtime")
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
  add_zlink_framework_sample_runner(
    ZoneWorld
    "framework-sample-zoneworld;framework-sample-gateway;framework-sample-zone-node;framework-sample-ops"
    420)
endif()

if(ZLINK_FRAMEWORK_CPP_BUILD_TESTS AND ZLINK_FRAMEWORK_CPP_ENABLE_COVERAGE)
  find_program(ZLINK_GCOV_EXECUTABLE gcov REQUIRED)
  get_property(ZLINK_FRAMEWORK_CPP_TESTS DIRECTORY PROPERTY TESTS)
  zlink_framework_cpp_add_test(
    NAME test_cpp_framework_coverage_threshold
    COMMAND ${CMAKE_COMMAND}
      -D
      ZLINK_FRAMEWORK_CPP_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -D
      ZLINK_FRAMEWORK_CPP_SOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
      -D
      ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD=${ZLINK_FRAMEWORK_CPP_COVERAGE_THRESHOLD}
      -D
      ZLINK_GCOV_EXECUTABLE=${ZLINK_GCOV_EXECUTABLE}
      -P
      ${CMAKE_CURRENT_LIST_DIR}/tests/Zlink.Framework.Coverage/coverage_threshold.cmake
    LABELS "framework-coverage;framework-regression"
    TIMEOUT 120
    PROPERTIES
      DEPENDS "${ZLINK_FRAMEWORK_CPP_TESTS}")
endif()

# Anything this build tree produces on Windows is started directly -- by a
# sample's own run_sample.ps1, by CTest, or by a debugger -- and the loader
# resolves the Core runtime next to the image or not at all. Make every target
# wait for the staged copy so a partial build (`--build --target <sample>`,
# which is exactly what a sample runner issues) stages it too, not only a full
# build of ALL.
if(TARGET zlink_framework_cpp_stage_native_runtime)
  set(ZLINK_FRAMEWORK_CPP_STAGED_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
  foreach(sample_name IN LISTS ZLINK_FRAMEWORK_CPP_SAMPLE_NAMES)
    list(APPEND ZLINK_FRAMEWORK_CPP_STAGED_DIRECTORIES
      "${CMAKE_CURRENT_SOURCE_DIR}/samples/${sample_name}")
  endforeach()
  foreach(staged_directory IN LISTS ZLINK_FRAMEWORK_CPP_STAGED_DIRECTORIES)
    get_property(staged_targets DIRECTORY "${staged_directory}"
      PROPERTY BUILDSYSTEM_TARGETS)
    foreach(staged_target IN LISTS staged_targets)
      if(NOT staged_target STREQUAL "zlink_framework_cpp_stage_native_runtime")
        add_dependencies(${staged_target} zlink_framework_cpp_stage_native_runtime)
      endif()
    endforeach()
  endforeach()
endif()
