vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zlink-systems/zlink
    REF core/v1.2.0
    SHA512 0f6a0312f0f901b90b09ca77406058bbb7c44657866a574e5135ac380beefde69faa94ae9d0fea31e26e9d01266322a2b3dcb618e40eb8b2f5f80b5b53a5cdb1
    HEAD_REF main
)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(ZLINK_BUILD_SHARED ON)
    set(ZLINK_BUILD_STATIC OFF)
else()
    set(ZLINK_BUILD_SHARED OFF)
    set(ZLINK_BUILD_STATIC ON)
endif()

# One Boost tree per binary. Core compiles Boost.Asio and Beast into its own
# objects from ZLINK_BOOST_INCLUDE_DIR (core/CMakeLists.txt), and zlink-framework
# compiles the same headers out of vcpkg's boost-asio/boost-beast; both are
# installed side by side here, so both must be the same checkout -- the rule
# framework/languages/cpp/CMakeLists.txt already states for its own targets.
# Without this, Core falls back to its vendored core/external/boost, whose
# boost::asio symbols are weak and default-visible in a static libzlink.a (the
# version script that hides them applies to the shared library only). The two
# layouts then collapse onto one definition at link time and the consumer
# crashes at run time, after building and linking cleanly.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/core"
    OPTIONS
        -DZLINK_BOOST_INCLUDE_DIR=${CURRENT_INSTALLED_DIR}/include
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
