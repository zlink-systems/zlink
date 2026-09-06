# progress: P0 hotpath stream_tcp cell

Started. Reading hotpath_bench.cpp, hotpath_gate.py, 08-stream.ko.md, with_stream test scenario.

Design decision: no separate client OS thread. The raw BSD TCP client and the
STREAM socket calls both run in the single test thread, issued sequentially
(they are different fds; Core's own internal I/O threads still do the async
TCP work in the background exactly as for router_router_tcp). The
collect_scope_t RAII toggle is nested: the outer instance brackets the whole
measured loop (as in the other 4 cells) and a second, inner instance brackets
just the two raw POSIX socket calls per iteration, toggling collection off
around them and back on afterward. This avoids any cross-thread callgrind
accounting question entirely and keeps only Core/STREAM-side instructions
inside the counted window.

NOTIFY stays at its default (0): only one client ever connects and the
connection's routing id is learned from the first zlink_recv_part call, so no
length-0 connect/disconnect notification records are ever produced to filter
out.

Waiting for MACHINE_FREE marker before first build/valgrind run.

Implementation complete (hotpath_bench.cpp: run_stream_tcp + helpers +
main() dispatch; hotpath_gate.py: CELLS tuple). g++ -fsyntax-only against
core/include passed cleanly. No MACHINE_FREE marker yet as of first check
(22:1x) -- other files present (taskA.log, taskB_*.log, an.py, scope.py) but
not the marker. Waiting, polling every 60s, before first build/valgrind run.

Coordinator correction received: do not pause/resume collection around the
raw client send/recv (Callgrind's toggle is process-wide, and the STREAM
engine's decode/encode/write work happens on Core's I/O thread exactly while
this thread blocks in the client's recv -- pausing there would drop the
engine-side cost we most want to measure). Fixed: run_stream_tcp now uses a
single collect_scope_t around the whole measured loop, matching
router_router_tcp's pattern; removed the nested pause blocks. Re-verified
with g++ -fsyntax-only (clean). Will verify after the first valgrind run
that engine symbols (asio_engine / raw_decoder / raw_encoder / pipe_t)
appear in the counted total via callgrind_annotate, per the coordinator's
instruction. Still waiting on MACHINE_FREE.

MACHINE_FREE appeared; built hotpath_bench (JOBS=6, clean). Manual callgrind
check (callgrind_annotate, and --separate-threads=yes) shows pipe_t
functions well-represented in the counted total, but asio_raw_engine_t /
raw_decoder_t / raw_encoder_t never appear (0 cost) -- and the SAME is true
for the existing, trusted router_router_tcp cell measured the same way, and
--separate-threads=yes shows 6 of 7 threads recording zero cost entirely.
So the io_thread/engine-side work is not reaching the counted total for any
TCP cell in this harness, not just stream_tcp; this is a pre-existing
characteristic of the gate, not a regression from this job's design.
Reported to coordinator instead of guessing at a fix (would require core/src
changes, out of scope).

Ran: --update-reference once (stream_tcp had no reference key), then 3x
stability + existing-4-cell check. All PASS within the gate's +/-5%
tolerance; stream_tcp's own run-to-run spread is ~1.2% (tighter than 5%,
looser than the brief's +/-0.5% aspiration) -- plausible real TCP loopback
syscall-count jitter, unlike the fully inproc/deterministic other cells.
Done; not committing per instructions.

Coordinator confirmed the zero-engine-cost finding is a real harness defect,
in scope: --collect-atstart=no + CALLGRIND_TOGGLE_COLLECT is per-thread, so
I/O threads never collect. Fix applied: hotpath_gate.py now passes
--instr-atstart=no (keep --collect-atstart default yes); collect_scope_t's
ctor/dtor now call CALLGRIND_START_INSTRUMENTATION /
CALLGRIND_STOP_INSTRUMENTATION (process-global) instead of
CALLGRIND_TOGGLE_COLLECT, same placement; added no-valgrind fallback macros
for both. g++ -fsyntax-only clean. Waiting up to 10 min for
'ninja|cmake --build|ctest|run_benchmarks' to clear (build-dev jobs running)
before rebuilding hotpath_bench and running valgrind, per coordinator's
machine-sharing note.

Fixed successfully; confirmed engine symbols now carry real cost for
stream_tcp under the new instrumentation scheme (callgrind_annotate:
asio_engine_t::{start_async_read,on_read_complete,process_input,
speculative_read/write,decode_and_push}, raw_decoder_t::decode,
encoder_base_t<raw_encoder_t>::encode all nonzero).

router_router_tcp check: zmp engine symbols (zmp_decoder_t/zmp_encoder_t/
asio_zmp_engine_t) still show ZERO cost even with the fix -- but for a
DIFFERENT, pre-existing reason unrelated to the per-thread instrumentation
bug: run_one_way's own design (unchanged, out of scope to touch) splits the
measured region into a collected SEND phase, an UNCOLLECTED 1-second sleep
("let the already-submitted batch reach the receive queue outside
collection"), then a collected RECEIVE phase. The async TCP engine's actual
decode work deliberately happens during that uncollected sleep gap by
design, so it was never going to show engine cost regardless of the
per-thread instrumentation fix. This is not something this job can or
should change (existing cell measurement code is off-limits). Reporting
this discrepancy plainly to the coordinator instead of glossing over it.

Also found and fixed a second harness bug caused by the same
--instr-atstart=no switch: hotpath_gate.py's read_instruction_total()
preferred "summary:" over "totals:", but with
CALLGRIND_START/STOP_INSTRUMENTATION the final dump's own "summary:" line
legitimately reads 0 (instrumentation is back off by the time the process-
exit dump fires) while "totals:" (the run's grand total) carries the correct
number. Every one of the 5 cells failed with "no positive summary/totals
instruction count" until this was fixed to prefer totals over summary.

Ran all 5 cells x 3 (flock-serialized against other jobs' builds). Updated
core/tests/perf/hotpath_reference.json to the median of the 3 runs for every
cell (all 5, per coordinator's instruction -- not just stream_tcp). 4th
verification run of all 5 cells against the new references: all PASS.
router_router_tcp's 2.262% spread (> 2%) reported as-is, not tuned away.
Done; not committing per instructions.
