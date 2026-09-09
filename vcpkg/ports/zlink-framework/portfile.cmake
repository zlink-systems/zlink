# Release 0.10.0 is currently incomplete: it requires an installed zlink_cpp
# package and generated protocol headers that its public assets do not supply.
# Do not substitute workspace sources for those missing release inputs.
# The release asset contains the contents of framework/languages/cpp directly.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/framework/v${VERSION}/zlink-framework-cpp-${VERSION}.tar.gz"
    FILENAME "zlink-framework-cpp-${VERSION}.tar.gz"
    SHA512 c53b8c84294cedf7cfa9e2b2cfe81e457a8b59b791cbcc50322a629bee0d4cf8016fc05b1cecc76ddc514ef8ad96da4c36daaf7735ae011d5122a8369ad5d977
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=ON
        -DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=0.17.6
        -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=0.17.5
        -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF
        -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=OFF
        -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF
        -DZLINK_FRAMEWORK_CPP_BUILD_E2E=OFF
        -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
        -DZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT=ON
        -DZLINK_STREAM_CONNECTOR_BUILD_UNREAL=OFF
        -DZLINK_STREAM_CONNECTOR_BUILD_GODOT=OFF
        -DZLINK_STREAM_CONNECTOR_BUILD_AXMOL=OFF
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX=${CURRENT_INSTALLED_DIR}
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=${CURRENT_INSTALLED_DIR}
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
foreach(package zlink_framework_cpp zlink_http_client_cpp zlink_stream_connector_cpp)
    vcpkg_cmake_config_fixup(PACKAGE_NAME "${package}" CONFIG_PATH "lib/cmake/${package}")
endforeach()
# Keep the release's component config names and expose the public umbrella name.
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/zlink_frameworkConfig.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/zlink_framework")
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CURRENT_PACKAGES_DIR}/share/zlink_framework/zlink_frameworkConfigVersion.cmake"
    VERSION "${VERSION}" COMPATIBILITY SameMinorVersion)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
