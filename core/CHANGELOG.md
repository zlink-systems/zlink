# libzlink Core Changelog

All notable changes to the Core library (`core/`, tags `core/vX.Y.Z`) are
recorded here, one section per version, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/). The public C ABI
(`core/include/**`, `core/src/libzlink.vers`) is unchanged across the 0.17
line unless a section says otherwise.

Release notes: the GitHub Release created by the `Build libzlink Core
Libraries` workflow for a `core/vX.Y.Z` tag embeds that version's section.
Design decisions are in `doc/plan/c016-worklog/decisions.ko.md` (D-B…).

## [Unreleased]

## [0.17.3] - unreleased

### Fixed

- STREAM sockets no longer stall with received frames left in the inbound
  pipes when one thread pulls packets while another thread sends on the same
  socket. The receive thread's lock-free public receive lease and the command
  owner's `activate_read` application could both modify the receive
  fair-queue; the receive ownership word is now the single gate for receive
  state access (command turn value, mutex-mode entrants re-validate after
  taking the mutex, async executor transitions happen under the mutex). The
  same race was latent for every fair-queue socket type and was what the
  `receive_once_guarded` / `check_read` TSan suppressions had been hiding.
  Regression test: `test_stream_concurrent_pull_send` (D-B218…D-B229).

## [0.17.2] - 2026-09-08

### Added

- Concurrent multipart submission on one socket: each calling thread
  assembles its own multipart record (per-caller slot created by the first
  successful `MORE`, removed on `FINAL`, discard or close). `FINAL` admits the
  whole record atomically through the complete-record path; other threads'
  incomplete records never collide. Applies to PAIR/DEALER/ROUTER SEND,
  REQUEST and REPLY. Contract wording clarified in the socket README (§2,
  part send, HWM, REPLY token), 07-router, 01-zmp control boundary and
  02-message (D-B197…D-B216).
- Regression tests for four-caller concurrent multipart on PAIR/DEALER/ROUTER,
  concurrent REQUEST/REPLY callers, thread-lifetime slot identity, and
  writable-resubmit from another thread while a sequence is open.

### Fixed

- Blocking `zlink_completion_recv(NONE)` on a socket with a completion poller
  registered now drives the transport completion drain itself (it observes
  the mailbox command epoch, drains, and waits on the same epoch with one
  `RCVTIMEO` budget across poller registration changes); previously the reply
  could stay in the transport until an unrelated socket progress.
- Discarded completion payloads (late reply with no pending request, payload
  export failure) are released outside the registry mutex and the physical
  send sync, so a zero-copy free callback may call socket APIs.
- The last user payload reference on REPLY failure or blocking REQUEST
  success is closed outside the physical send scope.
- Same-token duplicate REPLY checkout returns `INVALID_STATE`/`EBUSY` again;
  a token presented with a different RID is `NOT_FOUND`/`ENOENT`; logical RID
  removal returns registry capacity exactly once.

### Performance

- Session-side `pipe_t::write`/`flush` no longer take `_out_sync`; the
  peer-consumed counters are published as a seqlock-guarded C3 ledger and the
  monitor snapshot reads both pairs coherently (G-11b, `5304885197`).
- Every command drain takes the socket turn (the PAIR fast-activation
  exception is gone); the asynchronous executor's mailbox wakes through the
  asio post alone while no public FD user is registered; the immediately
  admitted send no longer reads the clock; `msg_t` hot accessors are inlined;
  two hot-path locks that guarded nothing were removed (G-1, G-3, G-7, G-10,
  G-11a, G-2).
- Hot-path reference cells (Release+LTO, callgrind Ir/msg):
  `dealer_router_reqrep_inproc` 18663 → 16455, `stream_tcp` 14623 → 13970.

### Changed

- C++ binding contract test updated: a second thread's multipart submit now
  succeeds instead of `invalid_argument`.

## [0.17.1] - 2026-09-07

Version pin for the bindings performance campaign; no public interface
change. Contains the STREAM/per-message cost work landed since 0.17.0:

### Fixed

- `zlink_close` drains a transient in-flight public-API admission before
  reporting `CLOSE_BUSY` (poller-thread race).
- Invalid send flags now fail with `EINVAL` as the errors chapter specifies
  (previously `ENOTSUP`).

### Performance

- STREAM raw engine no longer prepares gather output it cannot use; TLS
  lookups use the initial-exec model; the socket's public receive path takes
  two locks instead of three and uses a non-recursive mutex; the pipe's
  `activate_read` handling no longer takes `_out_sync`; `pipe_t::_in_active`
  and `_state` are atomics so the command owner and the I/O thread do not
  share a lock for them (S-1, S-2, S-4, S-9, S-10, S-11, S-12).

### Removed

- Phase 3 cleanups with no behaviour change: STREAM socket and asio engine
  dead paths, pipe/session and socket_base duplication, API/registry/
  transport/option helpers (R1–R9, R7/R11), ~2,700 lines net.

## [0.17.0] - 2026-09-06

Baseline of the 0.17 line. Public disconnect/unbind drains the socket's
queued commands under the API lock before removing the endpoint; a received
peer weight has one owner (the pipe owner command); Core tests are
interface-based (integration/contract executables use only the public C API,
linking the shared library).
