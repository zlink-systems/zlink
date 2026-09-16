[English](./bindings-cpp-1.2.0.md) | [한국어](./bindings-cpp-1.2.0.ko.md)

# ZLink C++ binding 1.2.0 release notes

This release runs on Core 1.2.0. The 1.1.0 binding assumes the Core 1.1.0 ABI and cannot be used with Core 1.2.0; this release closes that gap.

## Changes

- Binds to the Core 1.2.0 native library. The native library and provenance in the package are the Core 1.2.0 release assets.
- Fixed the error projection that read a typed result code as an errno (#355).
- The installed config is also found in the vcpkg layout.
## Verification

- The binding test suite passes on the Core 1.2.0 package.

The release tag is `cpp/v1.2.0`.
