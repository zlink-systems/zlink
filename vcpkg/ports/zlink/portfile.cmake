vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zlink-systems/zlink
    REF core/v1.1.0
    SHA512 b3a6ea5dc889c6741a479dcdbe4f2ac673f0fedde80af239bd40c0636aaa1342648cc7e29015569a62cc33261860f3791dbfe35931183b83a670e7b6a6941787
    HEAD_REF main
)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(ZLINK_BUILD_SHARED ON)
    set(ZLINK_BUILD_STATIC OFF)
else()
    set(ZLINK_BUILD_SHARED OFF)
    set(ZLINK_BUILD_STATIC ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/core"
    OPTIONS
        -DBUILD_SHARED=${ZLINK_BUILD_SHARED}
        -DBUILD_STATIC=${ZLINK_BUILD_STATIC}
        -DENABLE_LTO=OFF
        -DBUILD_TESTS=OFF
        -DZLINK_BUILD_TESTS=OFF
        -DBUILD_BENCHMARKS=OFF
        -DZLINK_BUILD_CPP_BINDINGS=OFF
        -DZLINK_CMAKECONFIG_INSTALL_DIR=lib/cmake/zlink
        -DWITH_DOC=OFF
        -DENABLE_CPACK=OFF
        -DWITH_TLS=ON
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin" "${CURRENT_PACKAGES_DIR}/bin")
endif()
vcpkg_cmake_config_fixup(PACKAGE_NAME zlink CONFIG_PATH lib/cmake/zlink)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

if(EXISTS "${SOURCE_PATH}/LICENSE")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
elseif(EXISTS "${SOURCE_PATH}/COPYING")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
else()
    message(FATAL_ERROR "No license file found in source tree")
endif()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
