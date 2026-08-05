vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL https://github.com/ulala-x/zlink.git
  REF framework-cpp-stream-connector/v${VERSION}
)

file(INSTALL
  "${SOURCE_PATH}/framework/languages/cpp/connector/e2e-client/include/"
  DESTINATION "${CURRENT_PACKAGES_DIR}/include"
  FILES_MATCHING PATTERN "*.hpp")

set(PACKAGE_CONFIG_DIR "${CURRENT_PACKAGES_DIR}/share/zlink_stream_e2e_client")
file(MAKE_DIRECTORY "${PACKAGE_CONFIG_DIR}")
file(WRITE "${PACKAGE_CONFIG_DIR}/zlink_stream_e2e_clientConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(zlink_stream_connector_cpp CONFIG)

if(NOT TARGET zlink::stream_e2e_client)
  get_filename_component(_zlink_stream_e2e_client_prefix
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
  add_library(zlink::stream_e2e_client INTERFACE IMPORTED)
  set_target_properties(zlink::stream_e2e_client PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_zlink_stream_e2e_client_prefix}/include"
    INTERFACE_LINK_LIBRARIES "zlink::stream_connector;zlink::stream_connector_codecs")
endif()
]=])

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
