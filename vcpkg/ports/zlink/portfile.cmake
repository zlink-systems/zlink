# Core exposes a pure C API: core/include/zlink.h and everything it includes
# name no C++ and no Boost type. Install the shared library published with the
# Core release tag instead of compiling Core here -- it was the longest step of
# the three-port build. zlink-cpp and zlink-framework do put C++ types on their
# boundary and keep building from source.
set(ZLINK_RELEASE_TAG "core/v1.2.0")
set(ZLINK_RELEASE_VERSION "1.2.0")

# Which prebuilt archive fits this triplet, and why not when none does. The
# archives are built by the Core release workflow for these four targets only.
set(ZLINK_ARCHIVE_PLATFORM "")
set(ZLINK_NO_ARCHIVE_REASON "the ${TARGET_TRIPLET} triplet has no prebuilt Core archive")
if(NOT VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    # Only the shared library is safe to take prebuilt out of the release this
    # port pins. Its version script exports the 99 zlink_* entry points and
    # nothing else, so the Boost that Core was built with cannot reach the
    # consumer's link. The libzlink.a in the 1.2.0 archive has no such filter:
    # it carries ~2,400 boost::* definitions compiled from Core's own Boost
    # 1.85. Linked next to zlink-framework, which compiles vcpkg's Boost 1.92
    # from the same headers, the two collapse onto one definition and the
    # consumer segfaults inside service_registry::do_use_service on its first
    # bind. So a static triplet keeps building Core from source, against the
    # very Boost installed here -- that shared tree is what makes the collapse
    # harmless.
    #
    # Core 1.3.0 gives libzlink.a the same public surface as the shared library
    # on Linux and macOS (issue #418), so this gate can be lifted for those two
    # once ZLINK_RELEASE_TAG points at 1.3.0 or later. Lifting it is more than
    # deleting these lines: the archive-installing branch below copies only the
    # shared library and drops the libzlink-static target from the CMake
    # package, and a static triplet needs the opposite. Windows stays here
    # either way -- MSVC has no way to localize a symbol in a static .lib, so
    # the archive's libzlink-v143-mt-s-<version>.lib still carries 3,616
    # defined Boost symbols.
    set(ZLINK_NO_ARCHIVE_REASON "a ${VCPKG_LIBRARY_LINKAGE} Core must be built against the Boost installed here")
elseif(VCPKG_TARGET_IS_LINUX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(ZLINK_ARCHIVE_PLATFORM "linux-x64")
        set(ZLINK_ARCHIVE_SHA512 "2f375e3967b1cf888c2c29858809d77e9ee46471e04f17d09b296e64acc06ac68c9c14e035b4555f596ce61897ef000bf3b9be54554d32ca50cee30a79114bff")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(ZLINK_ARCHIVE_PLATFORM "linux-arm64")
        set(ZLINK_ARCHIVE_SHA512 "117ad64954f5590038ded5adff0a8553205cb931704fec63e23f5c3ebf6dfa1bff45a9667e392d9deffecba56d8cbb4056d7075416555b04ddb2eefc0958f0e6")
    endif()
elseif(VCPKG_TARGET_IS_OSX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(ZLINK_ARCHIVE_PLATFORM "macos-arm64")
        set(ZLINK_ARCHIVE_SHA512 "543b57f09ad52d9acd166d7b730647cf682e384afcce4624b5e52e7ea67040f3c84502f47fee865a427d0f455bd182dd8230c359ccaf0820096895c8bb91c555")
    endif()
elseif(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW AND NOT VCPKG_TARGET_IS_UWP)
    # zlink.dll in the Windows archive is an MSVC v143 build against the
    # dynamic CRT, so a triplet on the static CRT would mix runtimes.
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64" AND VCPKG_CRT_LINKAGE STREQUAL "dynamic")
        set(ZLINK_ARCHIVE_PLATFORM "windows-x64")
        set(ZLINK_ARCHIVE_SHA512 "dff635879fdde5cc7fb79a36919b358c35b11d29fff24615383932f2eb69f33df7c3872ef2e712edd8e5268c464481a43fba595664dca3073fb1604e21f378c8")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(ZLINK_NO_ARCHIVE_REASON "zlink.dll in the prebuilt Core archive needs the dynamic CRT, and this triplet asks for CRT ${VCPKG_CRT_LINKAGE}")
    endif()
endif()

if(NOT ZLINK_ARCHIVE_PLATFORM)
    message(STATUS "zlink: ${ZLINK_NO_ARCHIVE_REASON}; building Core from source.")
endif()

if(ZLINK_ARCHIVE_PLATFORM)
    vcpkg_download_distfile(ZLINK_ARCHIVE
        URLS "https://github.com/zlink-systems/zlink/releases/download/${ZLINK_RELEASE_TAG}/libzlink-${ZLINK_ARCHIVE_PLATFORM}.tar.gz"
        FILENAME "libzlink-${ZLINK_RELEASE_VERSION}-${ZLINK_ARCHIVE_PLATFORM}.tar.gz"
        SHA512 "${ZLINK_ARCHIVE_SHA512}"
    )
    vcpkg_extract_source_archive(ZLINK_PREFIX ARCHIVE "${ZLINK_ARCHIVE}")

    # The archive is one `make install` prefix, so take its layout as given and
    # fail loudly if a future release moves something.
    foreach(required IN ITEMS "include/zlink.h" "lib/cmake/zlink/zlinkConfig.cmake"
            "lib/cmake/zlink/zlinkTargets.cmake" "lib/cmake/zlink/zlinkTargets-release.cmake"
            "lib/pkgconfig/libzlink.pc" "share/zlink/LICENSE.txt")
        if(NOT EXISTS "${ZLINK_PREFIX}/${required}")
            message(FATAL_ERROR "The Core ${ZLINK_RELEASE_VERSION} ${ZLINK_ARCHIVE_PLATFORM} archive has no ${required}.")
        endif()
    endforeach()

    # Only the shared library is installed (see the linkage gate above), so the
    # static archive next to it in the same prefix is left where it is.
    set(ZLINK_KEEP_TARGET "libzlink")
    set(ZLINK_DROP_TARGET "libzlink-static")
    if(VCPKG_TARGET_IS_WINDOWS)
        file(GLOB ZLINK_LINK_FILES "${ZLINK_PREFIX}/lib/zlink.lib")
        file(GLOB ZLINK_RUNTIME_FILES "${ZLINK_PREFIX}/bin/zlink.dll")
    elseif(VCPKG_TARGET_IS_OSX)
        file(GLOB ZLINK_LINK_FILES "${ZLINK_PREFIX}/lib/libzlink*.dylib")
        set(ZLINK_RUNTIME_FILES "")
    else()
        file(GLOB ZLINK_LINK_FILES "${ZLINK_PREFIX}/lib/libzlink.so*")
        set(ZLINK_RUNTIME_FILES "")
    endif()
    if(NOT ZLINK_LINK_FILES)
        message(FATAL_ERROR "The Core ${ZLINK_RELEASE_VERSION} ${ZLINK_ARCHIVE_PLATFORM} archive has no shared library.")
    endif()

    file(COPY "${ZLINK_PREFIX}/include" DESTINATION "${CURRENT_PACKAGES_DIR}")

    # The release archive is the only build Core publishes. vcpkg's default
    # triplets install a debug tree next to the release one and check that the
    # two hold the same binaries, so the release files are copied into both and
    # the debug import configuration below points at the debug copy. There is
    # no separately compiled debug Core to put there.
    file(COPY ${ZLINK_LINK_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
    file(COPY ${ZLINK_LINK_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
    if(ZLINK_RUNTIME_FILES)
        file(COPY ${ZLINK_RUNTIME_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
        file(COPY ${ZLINK_RUNTIME_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
    endif()

    # The archive carries the CMake package Core's own install(EXPORT) wrote, so
    # use it rather than re-deriving one here. It declares both imported targets
    # -- libzlink and libzlink-static -- because Core builds both, and a
    # generated targets file stops find_package() with a fatal error over an
    # imported location that is not there. Drop libzlink-static, whose library
    # this port deliberately does not install.
    function(zlink_write_package_config config_dir configuration)
        file(MAKE_DIRECTORY "${config_dir}")
        foreach(name IN ITEMS zlinkConfig.cmake zlinkConfigVersion.cmake zlinkTargets.cmake)
            configure_file("${ZLINK_PREFIX}/lib/cmake/zlink/${name}" "${config_dir}/${name}" COPYONLY)
        endforeach()
        file(READ "${config_dir}/zlinkTargets.cmake" contents)
        string(REGEX REPLACE "(foreach\\(_cmake_expected_target IN ITEMS)[^)]*\\)" "\\1 ${ZLINK_KEEP_TARGET})" contents "${contents}")
        string(REGEX REPLACE "# Create imported target ${ZLINK_DROP_TARGET}\n[^#]*" "" contents "${contents}")
        if(contents MATCHES "add_library\\(${ZLINK_DROP_TARGET} ")
            message(FATAL_ERROR "Could not drop ${ZLINK_DROP_TARGET} from the Core CMake package; its generated shape changed.")
        endif()
        file(WRITE "${config_dir}/zlinkTargets.cmake" "${contents}")

        file(READ "${ZLINK_PREFIX}/lib/cmake/zlink/zlinkTargets-release.cmake" contents)
        string(REGEX REPLACE "# Import target \"${ZLINK_DROP_TARGET}\" for configuration \"Release\"\n[^#]*" "" contents "${contents}")
        if(contents MATCHES "TARGET ${ZLINK_DROP_TARGET} ")
            message(FATAL_ERROR "Could not drop ${ZLINK_DROP_TARGET} from the Core CMake import file; its generated shape changed.")
        endif()
        if(configuration STREQUAL "Debug")
            # vcpkg_cmake_config_fixup rewrites "${_IMPORT_PREFIX}/lib" to
            # "${_IMPORT_PREFIX}/debug/lib" in *-debug.cmake, so this points at
            # the debug copy made above.
            string(REPLACE "RELEASE" "DEBUG" contents "${contents}")
            string(REPLACE "\"Release\"" "\"Debug\"" contents "${contents}")
        endif()
        string(TOLOWER "${configuration}" configuration)
        file(WRITE "${config_dir}/zlinkTargets-${configuration}.cmake" "${contents}")
    endfunction()
    zlink_write_package_config("${CURRENT_PACKAGES_DIR}/lib/cmake/zlink" "Release")
    zlink_write_package_config("${CURRENT_PACKAGES_DIR}/debug/lib/cmake/zlink" "Debug")
    vcpkg_cmake_config_fixup(PACKAGE_NAME zlink CONFIG_PATH lib/cmake/zlink)

    # The archive's .pc records the release runner's install prefix. Point it at
    # wherever vcpkg put this package instead.
    function(zlink_write_pkgconfig pc_path prefix_up libdir)
        file(READ "${ZLINK_PREFIX}/lib/pkgconfig/libzlink.pc" contents)
        string(REGEX REPLACE "prefix=[^\n]*\nexec_prefix=[^\n]*\nlibdir=[^\n]*\nincludedir=[^\n]*"
            "prefix=\${pcfiledir}/${prefix_up}\nexec_prefix=\${prefix}\nlibdir=\${prefix}/${libdir}\nincludedir=\${prefix}/include"
            contents "${contents}")
        if(NOT contents MATCHES "prefix=\\\${pcfiledir}")
            message(FATAL_ERROR "Could not rewrite the prefix of the Core archive's libzlink.pc.")
        endif()
        file(WRITE "${pc_path}" "${contents}")
    endfunction()
    zlink_write_pkgconfig("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libzlink.pc" "../.." "lib")
    zlink_write_pkgconfig("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libzlink.pc" "../../.." "debug/lib")

    vcpkg_install_copyright(FILE_LIST "${ZLINK_PREFIX}/share/zlink/LICENSE.txt")
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO zlink-systems/zlink
        REF "${ZLINK_RELEASE_TAG}"
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

    # boost-asio and boost-beast are in vcpkg.json for this branch: it is the
    # branch every static triplet takes, and the default x64-linux triplet is
    # static.
    #
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

    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

    if(EXISTS "${SOURCE_PATH}/LICENSE")
        vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
    elseif(EXISTS "${SOURCE_PATH}/COPYING")
        vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
    else()
        message(FATAL_ERROR "No license file found in source tree")
    endif()
endif()

vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
