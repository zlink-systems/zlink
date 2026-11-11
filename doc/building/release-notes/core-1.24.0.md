[English](./core-1.24.0.md) | [한국어](./core-1.24.0.ko.md)

# libzlink 1.24.0 release notes

## Changes

- Release a message's last content reference in one place, so pooled zero-copy slice metadata of a nested view returns to its pool instead of leaking (#3918, #3930).
- Fix the underlying type of the send and receive flags enums to `unsigned int` under C++, so an unknown bit reaches the existing `EINVAL` check instead of undefined behavior. The C declaration, names, and signatures are unchanged (#3918, #3930).
- Admit a complete multipart record on an empty queue even when its `MORE` prefix exceeds a small HWM; before, such a record stayed backpressured even after the receiver resumed (#4118, #4135).

## Compatibility

There are no public API or ABI changes; linker SOVERSION remains 0. All seven binding versions move to 1.24.0. Framework versions remain 0.32.0. Component compatibility follows the [version policy](../versioning.md).
