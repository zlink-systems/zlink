# The transport and generated etcd types remain private to this extension.
find_package(OpenSSL REQUIRED)
set(ZLINK_ETCD_PROTO_ROOT "${ZLINK_FRAMEWORK_CPP_RUNTIME_DIR}/protocol/etcd/v3.6.16")
set(ZLINK_ETCD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ZLINK_ETCD_PROTOS
  gogoproto/gogo.proto
  google/api/http.proto
  google/api/annotations.proto
  protoc-gen-openapiv2/options/openapiv2.proto
  protoc-gen-openapiv2/options/annotations.proto
  etcd/api/authpb/auth.proto
  etcd/api/mvccpb/kv.proto
  etcd/api/versionpb/version.proto
  etcd/api/etcdserverpb/rpc.proto)
set(_etcd_proto_inputs "")
set(_etcd_generated_sources "")
set(_etcd_compilation_sources "")
foreach(proto IN LISTS ZLINK_ETCD_PROTOS)
  list(APPEND _etcd_proto_inputs "${ZLINK_ETCD_PROTO_ROOT}/${proto}")
  string(REGEX REPLACE "[.]proto$" ".pb.cc" source "${proto}")
  string(REGEX REPLACE "[.]proto$" ".pb.h" header "${proto}")
  list(APPEND _etcd_generated_sources "${ZLINK_ETCD_GENERATED_DIR}/${source}")
  list(APPEND _etcd_generated_headers "${ZLINK_ETCD_GENERATED_DIR}/${header}")
  # Static archive export restriction requires distinct member basenames.
  string(REPLACE "/" "_" compilation_source "${source}")
  set(compilation_source "${ZLINK_ETCD_GENERATED_DIR}/${compilation_source}")
  add_custom_command(
    OUTPUT "${compilation_source}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${ZLINK_ETCD_GENERATED_DIR}/${source}" "${compilation_source}"
    DEPENDS "${ZLINK_ETCD_GENERATED_DIR}/${source}"
    VERBATIM)
  list(APPEND _etcd_compilation_sources "${compilation_source}")
endforeach()
set(_etcd_grpc_source "${ZLINK_ETCD_GENERATED_DIR}/etcd/api/etcdserverpb/rpc.grpc.pb.cc")
set(_etcd_grpc_header "${ZLINK_ETCD_GENERATED_DIR}/etcd/api/etcdserverpb/rpc.grpc.pb.h")
add_custom_command(
  OUTPUT ${_etcd_generated_sources} ${_etcd_generated_headers}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ZLINK_ETCD_GENERATED_DIR}"
  COMMAND protobuf::protoc
    "--proto_path=${ZLINK_ETCD_PROTO_ROOT}"
    "--proto_path=$<JOIN:$<TARGET_PROPERTY:protobuf::libprotobuf,INTERFACE_INCLUDE_DIRECTORIES>,;--proto_path=>"
    "--cpp_out=${ZLINK_ETCD_GENERATED_DIR}"
    ${_etcd_proto_inputs}
  DEPENDS ${_etcd_proto_inputs} protobuf::protoc
  COMMAND_EXPAND_LISTS VERBATIM)
add_custom_command(
  OUTPUT "${_etcd_grpc_source}" "${_etcd_grpc_header}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ZLINK_ETCD_GENERATED_DIR}"
  COMMAND protobuf::protoc
    "--proto_path=${ZLINK_ETCD_PROTO_ROOT}"
    "--proto_path=$<JOIN:$<TARGET_PROPERTY:protobuf::libprotobuf,INTERFACE_INCLUDE_DIRECTORIES>,;--proto_path=>"
    "--grpc_out=${ZLINK_ETCD_GENERATED_DIR}"
    "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    "${ZLINK_ETCD_PROTO_ROOT}/etcd/api/etcdserverpb/rpc.proto"
  DEPENDS ${_etcd_proto_inputs} protobuf::protoc gRPC::grpc_cpp_plugin
  COMMAND_EXPAND_LISTS VERBATIM)

add_library(zlink_framework_locations_etcd ${ZLINK_FRAMEWORK_CPP_LIBRARY_TYPE}
  src/etcd.cpp src/encoding.cpp src/cursor.cpp src/client.cpp
  ${_etcd_compilation_sources} "${_etcd_grpc_source}")
add_library(zlink::framework_locations_etcd ALIAS zlink_framework_locations_etcd)
set_target_properties(zlink_framework_locations_etcd PROPERTIES
  EXPORT_NAME framework_locations_etcd)
zlink_framework_cpp_configure_shared_library(zlink_framework_locations_etcd)
zlink_framework_cpp_configure_msvc_target(zlink_framework_locations_etcd)
target_compile_features(zlink_framework_locations_etcd PUBLIC cxx_std_20)
target_include_directories(zlink_framework_locations_etcd
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE "${ZLINK_ETCD_GENERATED_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(zlink_framework_locations_etcd
  PUBLIC zlink::framework_provider_abstractions
  PRIVATE gRPC::grpc++ protobuf::libprotobuf OpenSSL::Crypto)
set(ZLINK_ETCD_GENERATED_DIR "${ZLINK_ETCD_GENERATED_DIR}" PARENT_SCOPE)
set(ZLINK_ETCD_GENERATED_HEADERS ${_etcd_generated_headers} "${_etcd_grpc_header}" PARENT_SCOPE)
