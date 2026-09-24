[한국어](./framework-cpp-0.24.1.ko.md) | [English](./framework-cpp-0.24.1.md)

# ZLink C++ Framework 0.24.1 Release Notes

Framework 0.24.1 uses C++ binding 1.7.0 and Core 1.7.0.

## Fixes

- Fixed the failure to load `libzlink` when a Godot C++ GDExtension is
  first imported into the editor on Linux with insufficient static TLS
  space. Core 1.7.0 no longer references libstdc++ TLS with the
  `initial-exec` model ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The source archive `.sha256` file records the archive file name after the checksum, without a directory path
  ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).

## Install

Choose the `linux-x64`, `linux-arm64`, `macos-arm64`, or `windows-x64` framework archive attached to the [`framework-cpp/v0.24.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.24.1), or run `bootstrap.cmake` to download the matching archive. Use `find_package(zlink_framework CONFIG REQUIRED)`. C++ binding 1.7.0 and Core 1.7.0 are required.

The release tag is [`framework-cpp/v0.24.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.24.1).
