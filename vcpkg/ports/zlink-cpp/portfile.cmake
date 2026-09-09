vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# This checksum identifies the currently published, header-only asset. The
# release workflow must publish the complete source unit and update this hash
# before this port can install from GitHub; do not skip checksum validation.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/cpp/v${VERSION}/zlink-cpp-${VERSION}.tar.gz"
    FILENAME "zlink-cpp-${VERSION}.tar.gz"
    SHA512 5c77a3260a8786f473ed7a002a1bb34e5668cc430a03bcfe1683ff5c1b304077befbecb6220f9e8f32743fa4599875eec3f7c8cb96c66579827570c24f8151b9
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")
foreach(required_path CMakeLists.txt cmake/zlink_cppConfig.cmake.in src/Runtime/Core/context.cpp include/zlink.hpp LICENSE)
    if(NOT EXISTS "${SOURCE_PATH}/${required_path}")
        message(FATAL_ERROR "Incomplete zlink-cpp source release: missing ${required_path}. Publish bindings/cpp with include/, src/, cmake/, CMakeLists.txt and the root LICENSE.")
    endif()
endforeach()

# The C++ binding itself is static; its Core dependency follows the triplet.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DZLINK_CPP_CORE_VERSION=0.17.5
        -DZLINK_CPP_BUILD_TESTS=OFF
        -DZLINK_CPP_BUILD_SAMPLES=OFF
        -DZLINK_CPP_BUILD_BENCHMARKS=OFF
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME zlink_cpp CONFIG_PATH lib/cmake/zlink_cpp)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
