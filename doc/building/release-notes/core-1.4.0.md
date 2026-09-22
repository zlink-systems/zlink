[English](./core-1.4.0.md) | [한국어](./core-1.4.0.ko.md)

# libzlink 1.4.0 release notes

The C API and ABI are unchanged from 1.3.0 (`LIBZLINK_ABI_SOVERSION=0`). This
release makes the macOS Core dylibs independent of the release build path.

## What changed

**The macOS archive contains only loader-relative `LC_RPATH` entries.** The
release build removes every absolute `LC_RPATH` left by CMake and adds
`@loader_path`. It verifies the resulting Mach-O load commands before signing.
Package verification rejects an absolute or otherwise non-relative `LC_RPATH`.

## Impact on consumers

The public C API and ABI do not change, so existing Core consumers need no
source changes. macOS consumers can move an unpacked 1.4.0 archive without its
dylibs retaining a path to the release build machine.

## Verification

The macOS release build and package verification inspect every packaged dylib's
`LC_RPATH` entries. A non-relative entry fails the build before an archive is
produced.

The release tag is [`core/v1.4.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.4.0).

Related issue: [#962](https://github.com/zlink-systems/zlink/issues/962)
