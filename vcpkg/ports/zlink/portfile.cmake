vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/ulala-x/zlink.git
    REF core/v${VERSION}
)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(ZLINK_BUILD_SHARED ON)
    set(ZLINK_BUILD_STATIC OFF)
else()
    set(ZLINK_BUILD_SHARED OFF)
    set(ZLINK_BUILD_STATIC ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED=${ZLINK_BUILD_SHARED}
        -DBUILD_STATIC=${ZLINK_BUILD_STATIC}
        -DBUILD_TESTS=OFF
        -DZLINK_BUILD_TESTS=OFF
        -DZLINK_BUILD_CPP_BINDINGS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME zlink)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

if(EXISTS "${SOURCE_PATH}/LICENSE")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
elseif(EXISTS "${SOURCE_PATH}/COPYING")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
else()
    message(FATAL_ERROR "No license file found in source tree")
endif()
