[English](./bindings-java-1.5.0.md) | [한국어](./bindings-java-1.5.0.ko.md)

# ZLink Java binding 1.5.0 release notes

This release aligns the Java binding package with Core 1.5.0.

## Changes

- The Java binding package version is 1.5.0 for the Core 1.5.0 release.
- Core 1.5.0 fixes loading `libzlink` with late `dlopen()` in hosts with little static TLS surplus. The Beast WebSocket secure PRNG now stores its per-thread state in a heap object behind an 8-byte owning pointer instead of a 112-byte `thread_local` object, reducing PT_TLS usage from 312 to 200 bytes. The public C API and ABI are unchanged.
- This release changes no Java binding behavior; it aligns the binding version with Core 1.5.0.

## Verification

- The release preflight verifies the Java binding version, release notes, and Maven package metadata.

The release tag is `java/v1.5.0`.

