[English](./core-1.2.0.md) | [한국어](./core-1.2.0.ko.md)

# libzlink 1.2.0 release notes

The C API and ABI are unchanged from 1.1.0 (`LIBZLINK_ABI_SOVERSION=0`). This release
changes **what the distributed archives contain**.

## What changed

**The archives carry the whole install prefix.** They previously held one shared
library and three public headers. They now also carry `lib/cmake/zlink/*.cmake`, the
static library, `lib/pkgconfig`, and the full header tree. Point
`CMAKE_PREFIX_PATH` at an unpacked archive and `find_package(zlink CONFIG REQUIRED)`
resolves.

That makes it possible to **consume Core as a download rather than a source build**.
The C++ binding and framework must still be built against the consumer's own
configuration because they expose C++ types across the boundary; Core exposes a pure
C API and carries no such constraint.

**The static library is no longer bound to one toolchain.** Earlier builds enabled
LTO globally, so `libzlink.a` held GCC bytecode instead of real object code and only
linked under the exact compiler major version that produced it. LTO is now off for
the static target alone. The shared library is still built with it.

The Windows archive's CMake config moved from `CMake/` to `lib/cmake/zlink/`, so all
four platforms share one layout.

## Impact on consumers

Existing paths are unchanged. Tooling that reads `libzlink.so`, `libzlink.dylib`,
`bin/zlink.dll` or `include/*.h` from the archive root needs no change. This is an
addition, not a move.

## Verification

CI consumes the archives rather than inspecting them. All four platforms are checked
for the CMake config and the static library, and for LTO bytecode in any archive
member; linux-x64 unpacks its archive and links and runs a C program against both the
shared and static imported targets. That check runs on a runner with a **different GCC
major version** than the build job, so it proves the link across toolchains rather
than assuming it.

The release tag is [`core/v1.2.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.2.0).

Related issue: [#397](https://github.com/zlink-systems/zlink/issues/397)
