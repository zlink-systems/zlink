[English](./core-1.23.0.md) | [한국어](./core-1.23.0.ko.md)

# libzlink 1.23.0 release notes

## Changes

- Restart the poller before its final drain so handlers posted after `stop()` run, including deferred engine deletion (#3453, #3513).
- Transfer pending asynchronous commands to the inline owner when monitor command processing is released, without waiting for an idle turn (#3495).

## Compatibility

There are no public API or ABI changes; linker SOVERSION remains 0. All seven binding versions move to 1.23.0. Framework versions remain 0.31.0. Component compatibility follows the [version policy](../versioning.md).
