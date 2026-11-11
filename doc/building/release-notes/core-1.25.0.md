[English](./core-1.25.0.md) | [한국어](./core-1.25.0.ko.md)

# libzlink 1.25.0 release notes

## Changes

- Recheck the mailbox before detaching an idle asynchronous command owner, so a command arriving at that boundary is not left pending. This also fixes the first inproc send returning `EAGAIN` (#4186, #4234).
- Handle `SHUTDOWN` during close and preserve submit/close ordering in the bindings (#4060; Java #4193, .NET #4196, Node.js #4195).

## Compatibility

There are no public API or ABI changes; linker SOVERSION remains 0. Core and all seven binding versions are 1.25.0. Framework versions remain 0.32.0. Component compatibility follows the [version policy](../versioning.md).