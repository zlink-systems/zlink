[English](./core-1.15.0.md) | [한국어](./core-1.15.0.ko.md)

# libzlink 1.15.0 release notes

## Changes

- A `DONTWAIT` SEND or REQUEST wait token no longer has a deadline. It ends only on resource recovery (`ZLINK_SEND_ADMITTED`), explicit target removal (`ZLINK_SEND_NOT_FOUND`, STREAM `ZLINK_SEND_NOT_CONNECTED`), or socket close and context termination. `SNDTIMEO` applies only to a blocking `NONE` submission. Core no longer produces `ZLINK_SEND_TIMED_OUT` (803); the value is kept for ABI preservation. This reverts the wait-token deadline added in 1.11.0 (#1452).
- CMake compiler-flag probes now use valid result variable names, so `-Wall`, `-Wextra`, `-pedantic` (and MSVC `/W4`, `/WX`) and `LIBZLINK_WERROR` are applied; the reported warnings are fixed in source (#1420).
- Linux builds now detect `SO_BINDTODEVICE`, so `ZLINK_OPT_BINDTODEVICE` takes effect (#1420).
- Removed the internal socket priority path, which no public option could enable, and the unused `O_CLOEXEC` and TIPC build probes (#1420).
