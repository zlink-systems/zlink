# Framework archives are static; dependencies retain the triplet's linkage.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# SHA512 identifies the current incomplete release. Replace it after publishing
# the source unit produced by cmake/prepare-source-archive.cmake.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/framework/v${VERSION}/zlink-framework-cpp-${VERSION}.tar.gz"
    FILENAME "zlink-framework-cpp-${VERSION}.tar.gz"
    SHA512 c53b8c84294cedf7cfa9e2b2cfe81e457a8b59b791cbcc50322a629bee0d4cf8016fc05b1cecc76ddc514ef8ad96da4c36daaf7735ae011d5122a8369ad5d977
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")
if(NOT EXISTS "${SOURCE_PATH}/runtime/protocol/generated/cpp/service_wire_constants.hpp")
    message(FATAL_ERROR "The framework release archive is incomplete. Rebuild it with cmake/prepare-source-archive.cmake and update SHA512.")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=ON
        -DZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST=ON
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
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
foreach(package zlink_framework_cpp zlink_http_client_cpp zlink_stream_connector_cpp zlink_framework)
    vcpkg_cmake_config_fixup(PACKAGE_NAME "${package}" CONFIG_PATH "lib/cmake/${package}"
        DO_NOT_DELETE_PARENT_CONFIG_PATH)
endforeach()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib/cmake" "${CURRENT_PACKAGES_DIR}/debug/lib/cmake")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
