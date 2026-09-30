[English](./core-1.12.0.md) | [한국어](./core-1.12.0.ko.md)

# libzlink 1.12.0 release notes

## Changes

- STREAM sockets are bind-only. `zlink_connect` is rejected with `ZLINK_CONNECT_NOT_SUPPORTED` (602) and `ENOTSUP`. An accepted peer is disconnected with `zlink_disconnect_rid`; a RID that does not exist returns `ZLINK_CONNECT_NOT_FOUND` (605) (#1164).
- Fixed NODROP PUB/XPUB records ending partway through after the topic frame was written and the payload reached HWM. At record start, HWM is checked once for each pipe whose filter matches; an accepted record is written to completion (#1157).
