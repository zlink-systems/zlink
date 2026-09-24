[English](./core-1.5.0.md) | [한국어](./core-1.5.0.ko.md)

# libzlink 1.5.0 release notes

The public C API and ABI are unchanged from 1.4.0
(`LIBZLINK_ABI_SOVERSION=0`). This release reduces the static TLS required
when a host loads `libzlink` with `dlopen()`.

## What changed

The Beast WebSocket secure PRNG now uses a per-thread heap object behind an
owning pointer instead of a 112-byte `thread_local` object. This allows
`libzlink` to load in hosts with limited static TLS surplus, including the
reported Godot 4.4.1 .NET GDExtension configuration ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).

The hotpath benchmark links the shared library, and `ZLINK_BUILD_TESTS` is
defined only for test targets.

## Impact on consumers

Existing Core consumers need no source changes or ABI migration. Applications
that load `libzlink` late with `dlopen()` can use it when the host has limited
static TLS surplus.

## Verification

The hotpath gate runs its benchmark against the shared library. Test-only
definitions remain on test targets.

The release tag is [`core/v1.5.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.5.0).
