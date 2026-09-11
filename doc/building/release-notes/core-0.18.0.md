[English](./core-0.18.0.md) | [한국어](./core-0.18.0.ko.md)

# libzlink Core 0.18.0 Release Notes

Core 0.18.0 changes the public C ABI. SONAME compatibility with the 0.17 line is not preserved, and every binding restarts at 0.18.0.

## Highlights

- **Whole-message send/recv.** The caller passes a `zlink_msg_t[]` array with a count (send) or a capacity (receive) and one call moves one complete record. New: `zlink_send`, `zlink_send_rid`, `zlink_request`, `zlink_reply`, `zlink_publish`, `zlink_recv`, `zlink_router_recv`, `zlink_subscribe`, `zlink_xpub_recv` (renamed from `zlink_xpub_recv_part`). `zlink_stream_recv_packet` and `zlink_multipart_close` are unchanged.
- **Removed:** `zlink_send_part`, `zlink_send_part_rid`, `zlink_request_part`, `zlink_reply_part`, `zlink_publish_part`, `zlink_recv_part`, `zlink_router_recv_part`, `zlink_subscribe_part`, `zlink_xpub_recv_part`, and the public `zlink_part_flag_t` / `ZLINK_PART_MORE` / `ZLINK_PART_FINAL`. The partial-record state those calls left behind (the "first part to FINAL on one thread" rule, `BUSY`, partial retry) goes with them.
- A receive whose capacity is smaller than the record's part count returns `ZLINK_RECV_BUFFER_TOO_SMALL` (`ENOBUFS`) without consuming the record and writes the required part count; retrying with enough capacity receives that record exactly once.
- CPack NSIS icon path fixed.

Design and decisions: `doc/draft/core-whole-message-recv-api.ko.md` §7, `doc/plan/issue-63-worklog/decisions.ko.md` (D63-1..8). Issue #63, PR #86.

## Verification

- Release-gate build (LTO): ctest 214/214, public-surface check PASS (99 functions, exports match), binding contract tests PASS for six languages.
- Bindings perf (multi routed, tcp) improved over the 0.17.4 baseline in all four measured languages (cpp +8..+187 %, java +16..+240 %, dotnet +27..+187 %, node +18..+284 %); see `doc/plan/issue-63-worklog/perf-results.ko.md`.
- The `hotpath_gate` result for this tag is recorded in Issue #102.

## Migration

Replace per-part calls with one array-plus-count call. Binding public signatures did not change (only their internals moved to whole-message), so binding users only need to upgrade to binding 0.18.0. Direct C API users: see the send/recv sections of `core/doc/spec/core/socket/README.md`.

The release tag is [`core/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv0.18.0).
