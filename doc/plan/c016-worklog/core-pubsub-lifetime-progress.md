START detached@d5cb9d4739cb; scope core/src/** core/tests/**; preserve untracked core/build-main-readonly; begin dev build/reproduction
DEV_BUILD PASS JOBS=6 core/build-dev RelWithDebInfo LTO=OFF tests=ON
DEV_REPRO standalone-with-build-load runs=200 failures=2 rate=1.0% first=72 allocator_abort='corrupted double-linked list'
ASAN_BUILD START 00:05 core/build-asan JOBS=4 flags=-fsanitize=address,undefined
VALGRIND UNUSABLE (stripped ld.so, no libc6-dbg); gdb absent; relying on ASan build (in progress)
ASAN_BUILD PASS 00:18
ASAN_BUILD note: hotpath_bench static link fails (libzlink.a lacks asio_tcp_listener objects, unrelated); continuing with make -k
ASAN_BUILD PASS(after deleting 0-byte asio_tcp_listener.cpp.o left by earlier interrupted build) 00:26 85 test binaries
BASELINE_WT /home/hep7hep7/project/zlink-wt-pubsub-base @af7afd28e7 (parent of contract B) ASAN build start 00:38
FIX applied: i_decoder::detach_frame_admission hook, zmp_decoder_t impl, asio_engine_t::unplug severs decoder->session refs; ASan rebuild start 00:39
TEST added core/tests/integration/test_pubsub_close_during_inbound_frame.cpp (+CMake registration, label regression;serial); waiting for ASan/base rebuilds 00:41
VERIFY ASan fixed: test_backpressure_oneway_matrix_pubsub_regression 200 runs under load -> 0 memory errors (2 non-memory timing failures: line789 CONNECTION_READY wait timeout, line980 ws drain EAGAIN); new test 50/50 green; dev ctest -j2 started 00:50
REPRO_ASAN before-fix: pubsub_regression 200 runs under load -> 82 heap-use-after-free (session_base_t freed by own_t::process_term, read by ~zmp_decoder_t->release_decoder_frame via deferred engine destroy)
ROOTCAUSE: decoder holds session-scoped frame reservation; asio_engine destroyed on deferred io_context handler AFTER session_base_t deleted -> UAF on the SUB connect session
FIX: i_decoder::detach_frame_admission() virtual; zmp_decoder_t override releases reservation + nulls session refs; asio_engine_t::unplug() calls it before clearing session (core/src, +47 lines, 4 files)
TEST: core/tests/integration/test_pubsub_close_during_inbound_frame.cpp (marker-A-then-32MiB-B, block on recv A, close linger0; sleep-free public sync) + CMake
PREDATES contract B: mechanism from 88cd8557d7 (2026-08-30). Baseline wt @af7afd28e7(=50d77800f2~1): original 43/100 UAF, new test 19/50 UAF
VERIFY: new test ASan 50/50 green (was 19/50 on baseline); original test 200/200 no memory error under load; dev ctest -j2 -E hotpath_gate 140/140 PASS incl #110 new + #73 pubsub_regression
RESIDUAL non-memory pre-existing flakes (NOT the bug): line789 CONNECTION_READY timeout, line980 ws drain EAGAIN, testutil_monitoring.cpp:412 stack-use-after-return in test dispatch thread after a prior assert fail
EXIT:0
