# libzlink Core Changelog

All notable changes to the Core library (`core/`, tags `core/vX.Y.Z`) are
recorded here, one section per version, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/). Entries before
0.17.0 are in [`CHANGELOG-history.md`](./CHANGELOG-history.md). The public C ABI
(`core/include/**`, `core/src/libzlink.vers`) is unchanged across the 0.17
line unless a section says otherwise.

Release notes: the GitHub Release created by the `Build libzlink Core
Libraries` workflow for a `core/vX.Y.Z` tag embeds that version's section.
Design decisions are in `doc/plan/c016-worklog/decisions.ko.md` (D-B…).

## [Unreleased]

## [0.17.5] - 2026-09-09

### Fixed

- STREAM: accepting a connection reserved the pair's minimum Auto-HWM budget
  against the seed plan (queue count 0, i.e. only the profile's fixed cap), so
  once the fixed cap was consumed every further connection was refused with
  `ENOBUFS` (Balanced profile: about 3,500 connections). The reservation now
  tracks the reserved directions and admits against the effective budget of the
  topology being reserved, the same function the planner uses (spec
  06-auto-hwm §2 effective cap, atomic pair reservation). A STREAM echo server
  now reaches 10,000 TCP connections within the perf contract (connect
  concurrency 1024, ready timeout 10 s): 396.6 kops at 64 B, all six message
  sizes pass; 1,000-connection throughput unchanged (+0.55 %); ctest 213/213
  including the valgrind hotpath gate (machine A, D-BP52, `c72bc2dc63`).
- C perf runner: monitor event timing and errors, the client START-wait errno
  and a dirty-prefix provenance flag are recorded in the report so a failed
  connection barrier carries its cause.

## [0.17.4] - 2026-09-09

### Fixed

- Attaching a pipe no longer runs a synchronous, context-wide Auto-HWM
  replan (O(N) per attach, O(N²) per connection ramp): the attaching
  directions extend the last applied plan in O(log n) and the lowered targets
  of already-attached directions are recorded by the same debounced
  recalculation that option changes use. The convergence obligation is the
  debounce deadline itself (armed once per burst, cleared by the full pass),
  so a deferred shrink never re-triggers replanning. A STREAM echo server with
  4,000 connections on one I/O thread went from failing to accept in time to
  ~205 kops (spec 06-auto-hwm §2 row updated, D-H1; CCU-1…5).
- Context termination is published to every socket before monitor teardown,
  and blocking receive loops observe it on every turn (blocking send already
  did), so `zlink_ctx_shutdown()` interrupts a receive that is draining a large
  backlog instead of waiting for the drain (macOS exposed a ~13 ms window;
  MAC-4).
- REQUEST/REPLY: a reply of exactly 1,025 parts (one over the part-count
  boundary) no longer asserts (RR-1, machine A report D-BP43).
- macOS: `ipc://` bind works again (the `mkdtemp` probe was broken); wake,
  writev-lifetime and stream-packet-progress tests were made deterministic on
  the 3-core runner; the macOS build script gates on ctest (serial tests one at
  a time, then the parallel set) instead of tolerating failures (MAC-1…3).
- Bench (with_stream): the zlink STREAM echo scenario treated a normal TCP
  suffix after a complete frame as malformed; frames are now assembled per RID
  like the other stacks.

### Performance

- Unified socket lifecycle turn: command drain, receive lease and the async
  executor share one turn (receive-owner word, fallback mutex and
  `command_owner_sync` removed); socket-side `_out_sync` removed; route shard
  snapshot for ROUTER sends. STREAM TCP lock acquisitions per message
  15.1 → 8.4 (ALL-1/2).
- Large-payload receive path: the decoder/encoder read target grows on the
  first full read (2×, clamped to the existing maximum) instead of after two
  consecutive full reads, so a 64 KiB frame arrives in one read instead of two
  and the RAW echo fast path is kept. with_stream 64 KiB, 1,000 connections:
  32.3 → 40.3 kops (+25 %), p99 −24 %; peak server RSS +141 MiB at that cell
  (B2, D-B291).
- WS/WSS: a large ZMP body that crosses the encoder batch target is written in
  the same bounded-batch operation as the batch that holds its header instead
  of a separate WebSocket message; ws round-trip Q64/Q1 gate 0.55 → ≥ 1.3
  (DR ws 64 KiB 5.5 → ~30 kops) (SD-4).
- WS: 8-byte masking, batch/scratch sizing 16/64 KiB with growth to 128 KiB;
  large received payloads are built from the decoder's own recycle block
  instead of a copy; server RSS at 64 KiB ws −70 % (ALL-2b/3).
- Hot-path reference `dealer_router_reqrep_inproc` 16455 → 15610 Ir/msg.

### Removed

- Three STREAM gather environment variables that had no effect since the RAW
  engine stopped using gather writes (`ZLINK_ASIO_STREAM_DISABLE_GATHER`,
  `ZLINK_ASIO_STREAM_GATHER_THRESHOLD`, `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD`;
  spec 08-stream runtime defaults updated, D-f).

### Changed

- Linux release artifacts are built on Ubuntu 22.04 (glibc 2.35) instead of
  24.04, so the shipped `libzlink.so` no longer requires glibc 2.38
  (`__isoc23_strtol` and friends) and loads on Ubuntu 22.04 / Debian 12. The
  release provenance now records the highest glibc symbol version each Linux
  library needs.
- Windows random-byte entropy and context-termination `EAGAIN` fixes shipped
  in 0.17.3 are unchanged; Windows release gating is done by GitHub Actions
  (host Smart App Control blocks freshly built test binaries).
- Release workflow: Node 24 action majors; macOS Intel dropped (macOS ARM64,
  Linux x64/ARM64, Windows x64/ARM64 remain).

## [0.17.3] - 2026-09-08

### Fixed

- Windows: the random-byte generator had no entropy source (the `getrandom`
  and `/dev/urandom` paths were compiled out on Windows) and fell back to the
  15-bit, thread-local `rand()`, so unseeded I/O threads produced identical
  sequences and two transport pairs could receive the same 64-bit pair id; a
  stale lane fence then hit the fresh pair and READY never fired
  (`test_zmp_metadata` on the Windows CI runner). Windows now uses
  `RtlGenRandom` (`SystemFunction036`, resolved dynamically) and the `rand()`
  fallback is isolated.
- Context termination no longer aborts on a spurious mailbox wake
  (`wait_for_reaper_done` treated `EAGAIN` from a blocking receive as fatal).

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
