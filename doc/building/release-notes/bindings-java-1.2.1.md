[English](./bindings-java-1.2.1.md) | [한국어](./bindings-java-1.2.1.ko.md)

# ZLink Java binding 1.2.1 release notes

This release runs on Core 1.2.0. The binding code is the same as 1.2.0; only the package contents differ.

## Changes

- The jar includes the Windows x64 native library (`native/windows-x86_64/zlink.dll` and its dependent DLLs). The 1.2.0 jar carried only the Linux x86_64 native, so it could not load on Windows without `ZLINK_LIBRARY_PATH`.
- Binds to the Core 1.2.0 native library. The native library and provenance in the package are the Core 1.2.0 release assets.

## Verification

- The binding test suite passes on the Core 1.2.0 package.
- The published jar's `native/` entries were checked to contain both Linux x86_64 and Windows x64.

The release tag is `java/v1.2.1`.
