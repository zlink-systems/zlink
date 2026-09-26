# libzlink Core Changelog

All notable changes to the Core library (`core/`, tags `core/vX.Y.Z`) are
recorded here, one section per version, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/). Entries before
0.17.0 are in [`CHANGELOG-history.md`](./CHANGELOG-history.md). The public C ABI
(`core/include/**`, `core/src/libzlink.vers`) is stable from 1.0.0 on; before
that it was unchanged across the 0.17 line unless a section says otherwise.

Release notes: the GitHub Release created by the `Build libzlink Core
Libraries` workflow for a `core/vX.Y.Z` tag embeds that version's section.

## [Unreleased]

## [1.9.0] - 2026-09-26

The public C API and ABI are unchanged from 1.8.0
(`LIBZLINK_ABI_SOVERSION=0`).

### Changed

- `zlink_poller_wait` and `zlink_poll` no longer wait, for any timeout value,
  when nothing registered can become ready: no registration has non-zero
  `events`, or every such registration is a closed socket. The call returns a
  `POLLERR` not yet reported for a closed socket, or `0` with
  `ZLINK_CONFIG_OK`. This covers a poller with no registered source, which
  failed with `EFAULT` (`ZLINK_CONFIG_INVALID_HANDLE`) for a negative timeout
  and slept for a finite one, and a poller whose closed sockets had already
  reported `POLLERR` (#1087).
- A signal that interrupts `zlink_poll` or `zlink_poller_wait` no longer ends
  the call with `EINTR`. The wait continues until an event arrives or the
  original timeout expires (#1087).
- A signal that interrupts `zlink_ctx_term` no longer ends the call with
  `EINTR`. The call keeps waiting until every socket is closed, then
  succeeds (#1087).

### Fixed

- ROUTER no longer discards the pending DATA and REQUEST records of a selected
  route that ends without a successor; they stay receivable. They are
  discarded when another pipe is selected for the same routing id, together
  with a record of that routing id staged by a receive with too few part
  slots. This restores the 1.7.0 behavior that 1.8.0 broke (#1087).

## [1.8.0] - 2026-09-26

The public C API adds ROUTER route publication. Existing symbols and the ABI
are unchanged (`LIBZLINK_ABI_SOVERSION=0`).

### Added

- ROUTER publishes the route it selected for each routing id:
  `zlink_router_route_t`, `zlink_router_routes_snapshot`,
  `zlink_router_recv_route_generation` and the poll event
  `ZLINK_POLLROUTE = 64` (#1087).

### Changed

- ROUTER discards records from routes it did not select, at the selection
  change and in the common receive path. A discarded REQUEST does not receive
  a reply token (#1087).
- A blocking DEALER REQUEST submit that knows only weight-0 routes returns
  `NOT_ADMITTED` without waiting (#1087).
- The PUB/SUB conflate queue is a lock-free SPSC queue, and the writer ledger
  subtraction for replaced frames is made in one place. A replacing record now
  keeps the replaced record's position in receive order; it no longer moves to
  the tail, so the receive order across topics can differ from 1.7 (#1087).
- After `zlink_ctx_shutdown`, a poller wait on a socket source of that context
  ends without events with `ZLINK_CONFIG_INTERNAL_ERROR` (`ETERM`), and
  `zlink_completion_recv` ends with `ZLINK_RECV_TERMINATED`. Completions that
  were not yet received are discarded, as on socket close; 1.7 could still
  return already published completions depending on command-processing order
  (#1087).
- Registering `ZLINK_POLLROUTE` on a source that is not a ROUTER socket fails
  with `ENOTSUP` (#1087).
- Public endpoint disconnect waits for bind endpoint release outside the
  socket turn, and monitor lossy state is published with release/acquire
  (#1087).

### Fixed

- The fair queue no longer returns `ECONNABORTED` in place of the next pipe's
  record after a reject-consume leaves a multipart cursor behind (#1087).
- `term_endpoint` for a failed connect no longer terminates a bind endpoint
  with the same URI (#1087).

## [1.7.0] - 2026-09-24

The public C API and ABI are unchanged from 1.6.0
(`LIBZLINK_ABI_SOVERSION=0`).

### Fixed

- `libzlink` no longer references libstdc++ TLS with the `initial-exec`
  model. Core mutex and condition variable completion replace
  `std::promise` completion, so the first Godot editor import of a C++
  GDExtension on Linux can load the library (#1041).

### Build and tests

- CTest checks that `libzlink` has no `initial-exec` libstdc++ TLS
  references (#1041).

## [1.6.0] - 2026-09-24

The public C API and ABI are unchanged from 1.5.0
(`LIBZLINK_ABI_SOVERSION=0`).

### Fixed

- ROUTER no longer rejects reply-token publication with `ECONNABORTED`
  (`INTERNAL_ERROR 206`) when the source pipe disconnects after a complete
  REQUEST has been received. Physical disconnection does not invalidate the
  reply token (#1051).

## [1.5.0] - 2026-09-24

The public C API and ABI are unchanged from 1.4.0
(`LIBZLINK_ABI_SOVERSION=0`).

### Fixed

- `libzlink` can be loaded with `dlopen()` by hosts with limited static TLS
  surplus. The Beast WebSocket secure PRNG uses a per-thread heap object
  instead of a 112-byte `thread_local` object (#1041).

### Build and tests

- The hotpath benchmark links the shared library, and `ZLINK_BUILD_TESTS` is
  defined only for test targets (#1041).

## [1.4.0] - 2026-09-23

The public C API and ABI are unchanged from 1.3.0
(`LIBZLINK_ABI_SOVERSION=0`).

### Fixed

- The macOS release dylibs remove CMake's absolute `LC_RPATH` entries and add
  `@loader_path` instead. The build and package verification now reject any
  non-relative `LC_RPATH`, preventing a build-machine path from reaching a
  published archive (#962).

## [1.3.0] - 2026-09-22

The public C API and ABI are unchanged from 1.2.0
(`LIBZLINK_ABI_SOVERSION=0`). This release makes the distributed static and
macOS archives safe to consume in their intended configurations.

### Changed

- `libzlink.a` is restricted to the public `zlink_*` C ABI. Its vendored Boost
  implementation symbols are localized before publishing, so a consumer can
  link its own Boost version without collapsing Core and consumer definitions
  onto one symbol. The archive rewrite works on the GCC/binutils versions that
  build the release archives and on macOS; it fails the build rather than
  shipping an archive if that public-surface restriction cannot be established
  (#424, #433).
- The macOS arm64 release archive is a relocatable runtime closure. It bundles
  the required OpenSSL dylibs, rewrites Core and OpenSSL install names and
  dependencies to `@loader_path`, then ad-hoc signs every modified Mach-O
  binary. The package verification checks that closure, its signatures, and a
  clean C consumer before an archive is published (#855).

## [1.1.0] - 2026-09-14

Packaging fix for the Linux release archives. The library itself is
unchanged from 1.0.0: same public C API, same `libzlink.vers`, same
`libzlink.so.0` SONAME.

### Fixed

- The Linux release archives ship `libzlink.so` together with a
  `libzlink.so.0` symlink, the name the dynamic linker resolves. The
  1.0.0 archives instead carried a `libzlink.so.1` copy and no
  `libzlink.so.0`, so consumers that link against the archive directly
  (the C++ binding, the vcpkg port and the Conan recipe) failed at load
  time. Use 1.1.0 rather than 1.0.0 for those consumers; packages
  installed from npm, NuGet and Maven Central were unaffected.

### Changed

- The SONAME major is owned by `LIBZLINK_ABI_SOVERSION` in the root
  `VERSION` file. `core/CMakeLists.txt`, the Linux build script and the
  binding release workflow read it from there instead of deriving it
  from the package version. See `doc/building/versioning.ko.md`.

## [1.0.0] - 2026-09-14

First stable release. The public C API (`core/include/**`,
`core/src/libzlink.vers`) is **unchanged from 0.18.0**: no symbol was added,
removed or altered, so 0.18.0 sources compile and link against 1.0.0 without
edits. The SONAME moves from `libzlink.so.0` to `libzlink.so.1`, so consumers
relink against the new library. From this release on, the C ABI is stable and a
breaking change requires a new major version.

### Fixed

- Provisional byte accounting moves only the bytes a record owns, which fixes
  the high-water-mark underflow that let a socket report negative pending
  bytes (#309).
- Windows reconnect and poller lifecycle are stable: the poller no longer
  observes a half-closed socket after a reconnect (#210).
- A failing Windows `ctest` now fails the build instead of being reported as a
  successful run (#204).

### Changed

- The Conan recipe and the vcpkg port track the released Core version.
- Stream multiclient gating counts frame bytes rather than message counts in
  tests, which makes the gate independent of how a record is split.
- Five broken documentation anchors are corrected (#234).

## [0.18.0] - 2026-09-10

The public C ABI changes in this release (symbols removed and added; SONAME
compatibility with 0.17 is not preserved). Bindings restart at 0.18.0.

### Changed

- Send and receive are whole-message calls: the caller passes a `zlink_msg_t[]`
  array plus a count (send) or a capacity (receive) and one call moves one
  complete record. New public functions: `zlink_send`, `zlink_send_rid`,
  `zlink_request`, `zlink_reply`, `zlink_publish`, `zlink_recv`,
  `zlink_router_recv`, `zlink_subscribe`, `zlink_xpub_recv` (renamed from
  `zlink_xpub_recv_part`). `zlink_stream_recv_packet` and
  `zlink_multipart_close` are unchanged; STREAM RAW receive is `zlink_recv` with
  one part and STREAM send uses count 1. The per-part state a record spread over
  several calls used to leave behind (the "first part to FINAL on one thread"
  rule, `BUSY`, partial retry) no longer exists (Issue #63, PR #86;
  decisions D63-1..D63-8).
- Whole-message receive with a caller capacity smaller than the record's part
  count returns `ZLINK_RECV_BUFFER_TOO_SMALL` (`errno == ENOBUFS`) without
  consuming the record and writes the required part count; retrying with enough
  capacity receives the same record exactly once (D63-3). On success the first
  `count` slots of the caller array are caller-owned messages closed once with
  `zlink_multipart_close` (D63-4).
- Internal: `recv_router_message_direct` / `recv_dealer_message_direct` are
  `recv_router_record` / `recv_dealer_record` (D63-2).

### Removed

- `zlink_send_part`, `zlink_send_part_rid`, `zlink_request_part`,
  `zlink_reply_part`, `zlink_publish_part`, `zlink_recv_part`,
  `zlink_router_recv_part`, `zlink_subscribe_part`, `zlink_xpub_recv_part` and
  the public `zlink_part_flag_t` / `ZLINK_PART_MORE` / `ZLINK_PART_FINAL`.

### Fixed

- CPack NSIS icon path: the two `CPACK_NSIS_MUI_*ICON` lines now point at
  `core/packaging/installer.ico` (the previous `core/installer.ico` never
  existed).

### Verification

- Core ctest 214/214 on the release-gate build (LTO ON); public-surface check
  (`check_public_surface.py`) PASS, 99 functions, exports match. Binding contract
  tests for cpp, rust, java, node, dotnet, go PASS against this Core. The
  `hotpath_gate` result for this tag is recorded in Issue #102.
- Bindings perf (multi routed, tcp) versus the 0.17.4 baseline improved in all
  four measured languages (cpp +8..+187 %, java +16..+240 %, dotnet
  +27..+187 %, node +18..+284 %).

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
