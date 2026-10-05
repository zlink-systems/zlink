[English](./bindings-python-1.15.0.md) | [한국어](./bindings-python-1.15.0.ko.md)

# ZLink Python binding 1.15.0 release notes

Uses Core 1.15.0.

## Changes

- Follows the Core 1.15.0 wait-token contract: a `DONTWAIT` send or request waiting on backpressure no longer ends with a timeout; it completes on resource recovery, target removal, or socket close (#1452).
- A WRITABLE completion carrying the ABI-preservation value `ZLINK_SEND_TIMED_OUT` (803), which Core no longer produces, now projects to `INTERNAL_ERROR` with `EPROTO` like any other unknown value, instead of `BACKPRESSURED` with `EAGAIN` (#1452).
