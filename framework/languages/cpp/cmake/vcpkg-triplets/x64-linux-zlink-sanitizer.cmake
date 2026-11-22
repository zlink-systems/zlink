# x64-linux release packages built with the nightly ASan and UBSan flags.
# protobuf enables debug-only checks in generated code whenever a sanitizer is
# active, and those checks need a protobuf library built the same way (#5177).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS "-fsanitize=address,undefined -fno-sanitize=null -fno-omit-frame-pointer")
set(VCPKG_CXX_FLAGS "-fsanitize=address,undefined -fno-sanitize=null -fno-omit-frame-pointer")
set(VCPKG_LINKER_FLAGS "-fsanitize=address,undefined -fno-sanitize=null")
