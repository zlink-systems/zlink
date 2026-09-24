[한국어](./framework-cpp-0.25.0.ko.md) | [English](./framework-cpp-0.25.0.md)

# ZLink C++ Framework 0.25.0 Release Notes

Framework 0.25.0 uses C++ binding 1.7.0 and Core 1.7.0.

## Changes

- The C++ Stream Connector no longer depends on Core or the C++ binding. The byte type of the raw payload, typed codec, and compression codec interfaces changes from `zlink::message_t` to `std::vector<std::uint8_t>`. The connector package (CMake config, Conan, vcpkg) no longer requires Core, and the Godot, Unreal, and Axmol extensions no longer load `libzlink`. The connector public headers compile with exceptions disabled (`-fno-exceptions`, `/EHs-c-`) ([#1077](https://github.com/zlink-systems/zlink/issues/1077)).

## Fixes

- Fixed the failure to load `libzlink` when a Godot C++ GDExtension is
  first imported into the editor on Linux with insufficient static TLS
  space. Core 1.7.0 no longer references libstdc++ TLS with the
  `initial-exec` model ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The source archive `.sha256` file records the archive file name after the checksum, without a directory path
  ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- The sample READMEs run their commands from the sample directory, and the tutorial PowerShell POST example sends its JSON unescaped ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## Install

Choose the `linux-x64`, `linux-arm64`, `macos-arm64`, or `windows-x64` framework archive attached to the [`framework-cpp/v0.25.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.25.0), or run `bootstrap.cmake` to download the matching archive. Use `find_package(zlink_framework CONFIG REQUIRED)`. The server framework requires C++ binding 1.7.0 and Core 1.7.0, which the archive includes. A client that uses only the Stream Connector does not need Core or the binding.

The release tag is [`framework-cpp/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.25.0).
