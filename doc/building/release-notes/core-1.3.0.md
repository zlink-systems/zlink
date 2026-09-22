[English](./core-1.3.0.md) | [한국어](./core-1.3.0.ko.md)

# libzlink 1.3.0 release notes

The C API and ABI are unchanged from 1.2.0 (`LIBZLINK_ABI_SOVERSION=0`). This
release corrects the consumer boundary of the distributed static and macOS
archives.

## What changed

**The static archive exports only the public `zlink_*` C ABI.** Publishing
`libzlink.a` now localizes Core's vendored Boost implementation symbols. A
consumer can therefore link a different Boost version without Core and consumer
definitions collapsing onto one symbol. A toolchain that cannot establish this
restriction fails the build; it does not ship an unrestricted archive.

**The macOS arm64 archive is a relocatable runtime closure.** It bundles the
required OpenSSL dylibs and rewrites Core and OpenSSL install names and
dependencies to `@loader_path`. Every modified Mach-O binary is ad-hoc signed.
Package verification checks the closure, those signatures, and a clean C
consumer link and run before accepting the archive.

## Impact on consumers

The public C API and ABI do not change, so existing Core consumers need no
source changes. Consumers that link static Core alongside their own Boost should
use the 1.3.0 archive. macOS consumers can retain the unpacked archive as one
install prefix and use it from any location.

## Verification

The release build checks the public symbol surface of the archive. On macOS it
also checks the OpenSSL-containing dynamic-library closure, signatures, and a
clean C consumer. A failure prevents the release asset from being produced.

The release tag is [`core/v1.3.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.3.0).

Related issues: [#424](https://github.com/zlink-systems/zlink/issues/424),
[#433](https://github.com/zlink-systems/zlink/issues/433),
[#855](https://github.com/zlink-systems/zlink/issues/855)
