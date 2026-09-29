[English](./bindings-java-1.11.0.md) | [한국어](./bindings-java-1.11.0.ko.md)

# ZLink Java binding 1.11.0 release notes

Uses Core 1.11.0. WRITABLE TIMED_OUT (803) maps to BACKPRESSURED/EAGAIN, and resubmission occurs only for ADMITTED. Behavior that differed from Core now matches Core, removing binding-owned retry, retention, and timing rules (#1154).

## Changes

- The Java binding submits through Core outside the lock and removes its worker pool and default timeout (#1164).

