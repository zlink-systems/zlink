[English](./core-1.13.0.md) | [한국어](./core-1.13.0.ko.md)

# libzlink 1.13.0 release notes

## Changes

- Public data receives use the handle receive bit for admission; one call owns one record (#1292).
- Removed recursive trie traversal for long subscription topics to prevent Windows stack exhaustion (#1334).
- Direct engine termination emits a `DISCONNECTED` monitor event for the physical disconnect (#1334).
- Fixed READY processing reporting protocol errors for terminating ROUTER lanes (#1334).
- Corrected TSan instrumentation and classification of normal ROUTER pair termination (#1293).

## Retained contracts

These behaviors were included in 1.12.0 and remain unchanged in 1.13.0.

- STREAM sockets remain bind-only; accepted peers are disconnected with `zlink_disconnect_rid` (#1192).
- Wait tokens expire according to `SNDTIMEO`; exceeding the reservation limit returns `BACKPRESSURED`. Binding WRITABLE completions report timeout as `BACKPRESSURED` and `EAGAIN` (#1168).
- PUB/XPUB checks HWM once at publish-record start and writes accepted records to completion (#1183).
- The fixes for socket-close races, READY IDs, and monitor ordering remain in place (#1214).
- XSUB receive paths protect the subscription trie through the socket turn without a separate mutex (#1202).
