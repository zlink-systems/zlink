[English](./bindings-dotnet-1.19.1.md) | [한국어](./bindings-dotnet-1.19.1.ko.md)

# ZLink .NET binding 1.19.1 release notes

Uses Core 1.19.0.

## Changes

- Core stores payloads of 29 bytes or fewer in `zlink_msg_t`. When the .NET binding's `Message` stored this struct in a managed field that the GC can move, a compacting GC could leave the payload view pointing to an invalid address. This caused payload mismatches or a libcoreclr SIGSEGV. `Message` now stores `zlink_msg_t` in native memory and frees it once from Dispose or the finalizer. There is no public API change. Creating and disposing a `Message` without using a pool costs about 80 ns more (#1835).
