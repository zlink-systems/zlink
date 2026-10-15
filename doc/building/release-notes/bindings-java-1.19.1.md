[English](./bindings-java-1.19.1.md) | [한국어](./bindings-java-1.19.1.ko.md)

# ZLink Java binding 1.19.1 release notes

Uses Core 1.19.0.

## Changes

- Fixed a native memory leak on virtual threads. A `Message` closed on a virtual thread kept its native slot in that thread's thread-local pool, where it was neither reused nor released after the thread ended (one RouteMesh request leaked four shared arenas). Virtual threads now skip the thread-local slot and wrapper pools and return slots to the shared bounded pool, which closes slots beyond its capacity. The public API is unchanged (#1808, PR #1811).
