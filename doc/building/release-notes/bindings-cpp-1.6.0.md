[English](./bindings-cpp-1.6.0.md) | [한국어](./bindings-cpp-1.6.0.ko.md)

# ZLink C++ binding 1.6.0 release notes

This release aligns the C++ binding package with Core 1.6.0.

## Changes

- The C++ binding package version is 1.6.0 for the Core 1.6.0 release.
- Core 1.6.0 allows ROUTER to publish a reply token after a complete REQUEST is received, even if the source pipe disconnects. The public C API and ABI are unchanged ([#1051](https://github.com/zlink-systems/zlink/issues/1051)).
- This release changes no C++ binding behavior; it aligns the binding version with Core 1.6.0.

## Verification

- The release preflight verifies the C++ binding version, release notes, and Conan and vcpkg package metadata.

The release tag is `cpp/v1.6.0`.
