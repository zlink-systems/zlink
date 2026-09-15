# Framework archives are static; dependencies retain the triplet's linkage.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# SHA512 of the published framework-cpp/v0.12.0 source unit (cmake/prepare-source-archive.cmake).
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v${VERSION}/zlink-framework-cpp-${VERSION}.tar.gz"
    FILENAME "zlink-framework-cpp-${VERSION}.tar.gz"
    SHA512 493a55d4a8ddf859bbc057cdb75fce0e7d8b9eb7f1f7abd9d429ac1e5e0667645b481b828b46152167f4c62a3763538022436a44eb125d058d7ba810d916a992
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
        -DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=1.1.0
        -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=1.1.0
        # Without these two, ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_{CPP,CORE}_PREFIX
        # default to a repository-relative dev path (.artifacts/wsl/install/...)
        # that does not exist in a vcpkg tree; the install step that stages
        # the binding/Core CMake configs (framework/languages/cpp/CMakeLists.txt)
        # would then look in the wrong place instead of failing loudly.
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX=${CURRENT_INSTALLED_DIR}
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=${CURRENT_INSTALLED_DIR}
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
