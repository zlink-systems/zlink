# Framework archives are static; dependencies retain the triplet's linkage.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# SHA512 of the published framework-cpp/v0.20.0 source unit (cmake/prepare-source-archive.cmake).
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/framework-cpp/v${VERSION}/zlink-framework-cpp-${VERSION}.tar.gz"
    FILENAME "zlink-framework-cpp-${VERSION}.tar.gz"
    SHA512 d564db0f8bae8056a18ef798c5b5f4682da2fa05d01a2219f15ef13f7d9889aff1ee42942c941cf6c8eb42675fd1f7ec052404e5ff1e44b804da38a5e72320aa
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
        -DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=1.2.1
        -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=1.2.0
        # Without these two, ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_{CPP,CORE}_PREFIX
        # default to a repository-relative dev path (.artifacts/wsl/install/...)
        # that does not exist in a vcpkg tree; the install step that stages
        # the binding/Core CMake configs (framework/languages/cpp/CMakeLists.txt)
        # would then look in the wrong place instead of failing loudly.
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX=${CURRENT_INSTALLED_DIR}
        -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX=${CURRENT_INSTALLED_DIR}
        # zlink and zlink-cpp are dependencies of this port (vcpkg.json), so
        # they are already installed under ${CURRENT_INSTALLED_DIR} by the
        # time this port's install step runs. The StreamConnector component's
        # default staging of a *private* copy of their headers/libraries/
        # CMake configs (for standalone consumption outside a shared prefix)
        # is therefore both unnecessary and harmful here: re-staging "the"
        # include directory means re-staging every dependency's headers, not
        # just the binding's, and vcpkg's install step then refuses the
        # package for conflicting with whichever dependency owns them.
        -DZLINK_FRAMEWORK_CPP_STAGE_STANDALONE_DEPENDENCIES=OFF
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
