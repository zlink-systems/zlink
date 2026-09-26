# Framework archives are static; dependencies retain the triplet's linkage.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# SHA512 of the published framework-cpp/v0.25.0 source unit (cmake/prepare-source-archive.cmake).
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v${VERSION}/zlink-framework-cpp-${VERSION}.tar.gz"
    FILENAME "zlink-framework-cpp-${VERSION}.tar.gz"
    SHA512 aff5d0de15486267af313a90cc5d85f7a199996a8eb40e40706dc72eacba2af7d601db806223a20604205e1f86033c99d8ffc4ad292d8442680bf68c5ad2ab43
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")
if(NOT EXISTS "${SOURCE_PATH}/runtime/protocol/generated/cpp/service_wire_constants.hpp")
    message(FATAL_ERROR "The framework release archive is incomplete. Rebuild it with cmake/prepare-source-archive.cmake and update SHA512.")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=ON
        -DZLINK_FRAMEWORK_CPP_STAGE_STANDALONE_DEPENDENCIES=OFF
        -DZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST=ON
        -DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=1.7.0
        -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=1.9.0
        # Resolve the server framework's binding and Core packages from the
        # vcpkg prefix instead of the repository's local-package layout.
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX=${CURRENT_INSTALLED_DIR}
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=${CURRENT_INSTALLED_DIR}
        -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF
        -DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=OFF
        -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF
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
