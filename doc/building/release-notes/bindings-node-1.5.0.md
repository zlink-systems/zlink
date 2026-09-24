[English](./bindings-node-1.5.0.md) | [한국어](./bindings-node-1.5.0.ko.md)

# ZLink Node.js binding 1.5.0 release notes

This release aligns the Node.js binding package with Core 1.5.0.

## Changes

- The npm package version is 1.5.0 for the Core 1.5.0 release.
- Core 1.5.0 fixes loading `libzlink` with late `dlopen()` in hosts with little static TLS surplus. The Beast WebSocket secure PRNG now stores its per-thread state in a heap object behind an 8-byte owning pointer instead of a 112-byte `thread_local` object, reducing PT_TLS usage from 312 to 200 bytes. The public C API and ABI are unchanged.
- This release changes no Node.js binding behavior; it aligns the binding version with Core 1.5.0.

## Verification

- The release preflight verifies the Node.js binding version, release notes, and npm package metadata.

The release tag is `node/v1.5.0`.

