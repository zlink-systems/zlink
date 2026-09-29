[English](./bindings-dotnet-1.11.0.md) | [한국어](./bindings-dotnet-1.11.0.ko.md)

# ZLink .NET binding 1.11.0 release notes

Uses Core 1.11.0. WRITABLE TIMED_OUT (803) maps to BACKPRESSURED/EAGAIN, and resubmission occurs only for ADMITTED. Behavior that differed from Core now matches Core, removing binding-owned retry, retention, and timing rules (#1154).

## Changes

- The .NET binding submits through Core outside the socket lock and removes binding-owned defaults and result reclassification (#1164).

