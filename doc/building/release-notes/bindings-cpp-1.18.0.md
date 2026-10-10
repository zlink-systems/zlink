[English](./bindings-cpp-1.18.0.md) | [한국어](./bindings-cpp-1.18.0.ko.md)

# ZLink C++ binding 1.18.0 release notes

Uses Core 1.18.0.

## Changes

- Core 1.18.0 removes the cumulative O(n²) cost of rechecking all existing writable waiters when registering a waiter for a backpressured REQUEST. Registration checks only the new waiter; all correlation waiters are rechecked only when a correlation is actually released. In the public API measurement for #1590, registering 1,000 waiters took 0.918ms, down from 300.106ms (#1466, #1590).
