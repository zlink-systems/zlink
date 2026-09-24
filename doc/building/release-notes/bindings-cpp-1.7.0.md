[English](./bindings-cpp-1.7.0.md) | [한국어](./bindings-cpp-1.7.0.ko.md)

# ZLink C++ binding 1.7.0 release notes

This release aligns the C++ binding package with Core 1.7.0.

## Changes

- The C++ binding package version is 1.7.0 and requires Core 1.7.0.
- Core 1.7.0 fixes the failure to load `libzlink` when a Godot C++
  GDExtension is first imported into the editor on Linux with insufficient
  static TLS space ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The public C++ binding API and behavior are unchanged.

## Verification

- The release preflight verifies the C++ binding version, release notes, and Conan and vcpkg package metadata.

The release tag is `cpp/v1.7.0`.
