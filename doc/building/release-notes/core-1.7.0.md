[English](./core-1.7.0.md) | [한국어](./core-1.7.0.ko.md)

# libzlink 1.7.0 release notes

The public C API and ABI are unchanged from 1.6.0
(`LIBZLINK_ABI_SOVERSION=0`).

## What changed

`libzlink` no longer references libstdc++ TLS with the `initial-exec`
model. Completion through a Core mutex and condition variable replaces
`std::promise` completion. This fixes library loading during the first
Godot editor import of a C++ GDExtension on Linux when static TLS space
is insufficient ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).

## Impact on consumers

Existing Core consumers need no source changes or ABI migration. The Godot
editor can load `libzlink` during the first C++ GDExtension import on Linux.

## Verification

CTest checks that `libzlink` has no `initial-exec` libstdc++ TLS references.

The release tag is [`core/v1.7.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.7.0).
