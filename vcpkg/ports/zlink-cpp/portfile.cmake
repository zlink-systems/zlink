vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# This checksum belongs to the previous release asset. The release workflow
# must publish the complete 1.10.0 source unit and update this hash before
# this port can install from GitHub; do not skip checksum validation.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/zlink-systems/zlink/releases/download/cpp/v${VERSION}/zlink-cpp-${VERSION}.tar.gz"
    FILENAME "zlink-cpp-${VERSION}.tar.gz"
    SHA512 5445e2312191e702ecd9e0d796bb9cfa2a492c2927dea695cf0dbcd2502e56169880e4367b2242543749788b865ed653cd34cad13261103caad567c95e2f0844
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
        # ZLINK_CPP_CORE_VERSION은 주지 않는다. 아카이브의 CMakeLists.txt가 자기가 요구하는
        # Core 버전을 기본값으로 담고 있고 그 값은 sync-version.py가 소유한다. 여기서 다시
        # 적으면 릴리스마다 어긋난다(실제로 0.17.5에 멈춰 있었다).
        -DZLINK_CPP_CORE_PACKAGE_PREFIX=${CURRENT_INSTALLED_DIR}
        -DZLINK_CPP_BUILD_TESTS=OFF
        -DZLINK_CPP_BUILD_SAMPLES=OFF
        -DZLINK_CPP_BUILD_BENCHMARKS=OFF
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME zlink_cpp CONFIG_PATH lib/cmake/zlink_cpp)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/include/zlink/core"
    "${CURRENT_PACKAGES_DIR}/include/zlink/eventing"
    "${CURRENT_PACKAGES_DIR}/include/zlink/message"
    "${CURRENT_PACKAGES_DIR}/include/zlink/socket"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
