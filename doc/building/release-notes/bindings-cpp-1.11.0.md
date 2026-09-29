[English](./bindings-cpp-1.11.0.md) | [한국어](./bindings-cpp-1.11.0.ko.md)

# ZLink C++ binding 1.11.0 release notes

Uses Core 1.11.0. WRITABLE TIMED_OUT (803) maps to BACKPRESSURED/EAGAIN, and resubmission occurs only for ADMITTED. Behavior that differed from Core now matches Core, removing binding-owned retry, retention, and timing rules (#1154).

## Changes

- The C++ binding does not resubmit abandoned asynchronous sends, and limits the request-registration lock to map access (#1164).

