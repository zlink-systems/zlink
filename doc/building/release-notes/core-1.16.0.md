[English](./core-1.16.0.md) | [한국어](./core-1.16.0.ko.md)

# libzlink 1.16.0 release notes

## Changes

- Auto HWM removes runtime memory hint option 20; its snapshot field remains and is always zero. Core detects memory limits across the current cgroup and its parents. New connections are admitted against the resolved memory limit, including when a manual Core budget is set; that budget still controls HWM planning (#1476).
- Monitor ready counts remove the matching connection on disconnect. Closing a monitor during a poller wait now reports socket close instead of context termination (#1476, #1083).
- `ZLINK_OPT_BINDTODEVICE` is applied to `tcp`, `tls`, `ws` and `wss` sockets. A listener applies it before bind, and a connecter applies it before connect. The option had been stored without being applied since before 1.10.0. On platforms without interface binding support, setting a non-empty value returns `ZLINK_CONFIG_NOT_SUPPORTED` (#1469).
- `zlink_socket()` now returns `NULL` with the preserved errno, such as `EMFILE`, when the socket mailbox cannot create its signaler because the process has reached its file-descriptor limit. Previously, it returned a socket and the first `zlink_connect()` aborted with `Bad file descriptor` (#1481).
