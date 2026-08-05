[한국어](12-socket-options.ko.md)

# Socket options

Use `zlink_set_option()` and `zlink_get_option()` for common raw-socket
options. Router, dealer, stream, pub, and sub options use their typed accessors.
An unsupported option returns `ZLINK_CONFIG_NOT_SUPPORTED`.

## Configuration timing

Set routing ids, TLS credentials, transport buffers, and handshake-related
values before bind or connect. Runtime queue options may affect existing pipes
only as documented by the public contract.

## Queue and timeout options

- `ZLINK_OPT_SNDHWM` and `ZLINK_OPT_RCVHWM` bound the queued bytes. Values are
  `uint64_t` byte counts and `0` means unlimited. See the
  [socket spec](../spec/core/socket/README.en.md) for the exact accounting and
  errors.
- `ZLINK_OPT_SNDTIMEO` and `ZLINK_OPT_RCVTIMEO` bound blocking calls.
- `ZLINK_OPT_LINGER` controls how close treats pending outbound data.
- Automatic-HWM options select a profile and a per-message byte input.

## Transport options

TCP keepalive, reconnect intervals, kernel send/receive buffers, TOS, and TLS
settings affect transport behavior. Verify platform support before depending
on an option.

Read the resulting automatic-HWM plan through `zlink_monitor_status()` rather
than reconstructing the internal calculation in application code.
