# core PUBSUB lifetime double-free — investigation summary

Worktree: /home/hep7hep7/project/zlink-wt-pubsub-df (detached @d5cb9d4739, current main)
Scope touched: core/src/**, core/tests/** only. No git state changes, no commit.

## Repro rate

Dev (RelWithDebInfo, LTO off) standalone-under-build-load: 2/200 aborts (~1%),
glibc "corrupted double-linked list" / "double free or corruption (!prev)" (prior worker).

ASan+UBSan build (core/build-asan, RelWithDebInfo, LTO off, tests on, JOBS=4,
gcc 13.3 -fsanitize=address,undefined), test_backpressure_oneway_matrix_pubsub_regression:

- BEFORE fix, 200 runs under load: 82/200 aborted (~41%). Every one is the SAME
  heap-use-after-free (see stacks). (The much higher rate vs. dev is ASan
  serializing threads so the close/destroy race is hit far more often.)
- AFTER fix, 200 runs under load (x2 independent campaigns): 0 heap-use-after-free /
  double-free / corruption. Residual: 1–2 runs hit unrelated NON-memory flakes
  (see "Not the bug").

## Sanitizer stacks (the one bug)

heap-use-after-free, READ of 8 bytes, on a 1448-byte session_base_t:

  #0 session_base_t::release_decoder_frame(void*,void*)   session_base_pipe_io.cpp:153
  #1 zmp_decoder_t::release_frame_reservation()           zmp_decoder.cpp:105
  #2 ~zmp_decoder_t()                                      zmp_decoder.cpp:60
  #4 ~asio_engine_t()                                      asio_engine.cpp:230
  #5 ~asio_zmp_engine_t()
  #7 asio_engine_t::destroy_after_callbacks lambda         asio_engine.cpp:1946   (posted io_context handler)

freed by (earlier, same io thread):
  #1 operator delete                                       (session freed)
  #1 own_t::process_term(int)                              own.cpp:143
  #2 session_base_t::pipe_terminated(pipe_t*)              session_base.cpp:407
  #3 pipe_t::process_pipe_term_ack()                       pipe.cpp:2975

allocated by:
  session_base_t::create -> socket_base_t::create_resolved_connect_session
  -> connect_internal -> zlink_connect  (the SUB socket's connect session)

## Object and ownership rule violated

Object: the connect-side **session_base_t** (the SUB's session), read after free.

Mechanism (subscriber closed with linger 0 while a frame is mid-flight):

1. The ZMP decoder holds a per-frame *reservation* it acquires when a frame
   header arrives and releases through a back-pointer to its session
   (session_base_t::release_decoder_frame, wired by configure_zmp_decoder).
   The reservation storage itself lives inside session_base_t
   (_decoder_frame_reservation).
2. On close, the pipe term handshake completes on the io thread:
   pipe_terminated -> own_t::process_term **deletes the session_base_t**.
3. The engine is destroyed **later**, on a handler posted to the io_context by
   asio_engine_t::destroy_after_callbacks (deliberately deferred so in-flight
   SSL/WS callbacks drain). When that handler runs, ~asio_engine_t -> ~zmp_decoder_t
   still holds a live frame reservation (the payload never finished) and calls
   release_frame_reservation, which dereferences the already-freed session
   (subject pointer and the reservation object that lived inside it).

So the lifetime rule "the decoder must not touch the session after the session
is torn down" is violated: engine/decoder teardown is ordered *after* session
deletion, but the decoder still owned a session-scoped reservation.

Note asio_engine_t::terminate/unplug/~session_base_t already assume the engine
outlives nothing of the session — unplug() clears _connection_facade.session but
did not clear the decoder's independent back-pointer/reservation.

## Fix (minimal, core/src only, +47 lines)

Sever the decoder's session references while the session is still guaranteed
alive — inside asio_engine_t::unplug(), which is always called (from terminate()
and from ~session_base_t) before the session goes away:

- core/src/runtime/protocol/i_decoder.hpp: new virtual
  `void detach_frame_admission() {}` (no-op default; raw/stream decoders unaffected).
- core/src/runtime/protocol/zmp_decoder.{hpp,cpp}: override releases any held
  reservation through the still-live session, then nulls the admission
  handler/release-handler/subject so ~zmp_decoder_t can no longer call into it.
- core/src/runtime/engine/asio/asio_engine.cpp: in unplug(), call
  `_decoder->detach_frame_admission()` before `_connection_facade.session = NULL`.

The deferred ~zmp_decoder_t then finds no reservation and no subject: no session
access after free.

## Regression test (public API, sleep-free)

core/tests/integration/test_pubsub_close_during_inbound_frame.cpp (+CMake
registration, label "regression;serial", RESOURCE_LOCK network, RUN_SERIAL).
Two cases (SUB-first and XPUB-first close), 8 iterations each.

Deterministic public synchronisation, no sleeps: publish a small marker A then a
32 MiB frame B back-to-back; the subscriber blocks (RCVTIMEO) until it *receives A*
via zlink_subscribe. Because the engine writes A and B's frame header in one
batch and the decoder consumes B's header (taking the reservation) in the same
decode pass that delivers A, receiving A proves the decoder now holds B's
reservation while B's 32 MiB payload is still in flight. The socket is then
closed with linger 0 — squarely inside the window. Failure mode is a crash /
sanitizer report, never a wrong value.

## Verification

- ASan new test: 50/50 green on the fixed tree (both cases).
- ASan original failing test: 200 runs under load, 0 memory errors (was 82/200).
- Full dev ctest (`ctest --test-dir core/build-dev -j2 -E hotpath_gate`):
  140/140 passed, incl. the new test (#110) and pubsub_regression (#73). 203 s.
- Dev new test standalone: 10/10 green.

## Predates the recent commits — YES (evidence)

The reservation mechanism (release_decoder_frame) and the deferred engine
destruction (destroy_after_callbacks) were both introduced in 88cd8557d7
"feat(core): move request-reply metadata into ZMP headers" (2026-08-30), which is
*before* contract B (50d77800f2, 2026-09-04) and the other recent commits
(89ed9be356, bdf0917e14, 90b58fd213). Reproduced on baseline worktree
/home/hep7hep7/project/zlink-wt-pubsub-base @af7afd28e7 (= 50d77800f2~1, contract
B's parent), ASan:

- original test:      44/100 runs — 43 identical release_decoder_frame UAF (+1 harness flake).
- new regression test: 19/50 runs — all release_decoder_frame UAF.

Contract B did not introduce the bug; it (and ASan) only changed timing. The fix
is still applied on current main as required.

## Not the bug (residual non-memory flakes, pre-existing)

Under ASan+heavy load a few % of runs of the *original* backpressure test fail on
NON-memory conditions, independent of this fix and present on baseline too:

- test_backpressure_oneway_matrix.cpp:789 — CONNECTION_READY monitor wait times
  out (setup timing under load).
- test_backpressure_oneway_matrix.cpp:980 — "PUBSUB ws drain queued=1 ... Expected
  2 Was 1" EAGAIN (ws drain timing).
- testutil_monitoring.cpp:412 — stack-use-after-return in the test's own
  test_monitor_pull_dispatch receiver thread: when a Unity assertion above fails,
  the on-stack dispatch object is destroyed without the thread being stopped/joined
  first. A test-harness cleanup-ordering issue, only triggered *after* another
  assertion already failed. Not a libzlink lifetime bug.

These are test-side robustness issues; left as-is to keep the fix minimal and in
scope. Called out here so they are not mistaken for the memory bug.

## BLOCKERS

None for the fix. Environment notes:
- valgrind unusable here (stripped ld.so, no libc6-dbg); gdb absent — used ASan.
- An earlier interrupted ASan build left a 0-byte asio_tcp_listener.cpp.o; deleting
  it and rebuilding produced a clean tree. (hotpath_bench static-link failure is a
  pre-existing unrelated issue and does not affect the test binaries.)
- Baseline worktree /home/hep7hep7/project/zlink-wt-pubsub-base left in place as
  bisect evidence (git worktree add --detach; no commits).
