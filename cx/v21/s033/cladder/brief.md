# Job: bottom-up layer ladder — where C++ Framework request cost appears (0.33.0 #1639)

Model/level: gpt-6.1-sol high. 대표님 asked for this approach (2026-11-06): start from the binding level,
add Framework functions back one at a time, and see which step costs throughput.
No sub-agents or other models (no Astra). Do not run scripts/dev/internal-docs.sh or any git pull/fetch;
the previous job did, which violated its limits.

## Background
request-backpressure 1K: binding 246,826 msg/s, Framework 49,602 (0.20). Per-thread CPU µs/msg (Framework):
source caller 18.3 (binding caller ~4.1), source Core I/O 16.0, an unidentified source thread 15.7,
target receive 13.6, target handler workers 18.7, target Core I/O 16.2. 0.6 needs every serial thread
<= 6.75 µs. send-saturation 1K: 0.28, budget 2.70 µs. Sources: /root/jobs/fx/cx/v21/s033/cthin/report.md
(may still be in progress), csubmit/, cdiag/, coreio/ reports.

## Task
1. Rung 0: a standalone C++ request/reply echo using only the C++ binding (zlink-cpp 1.23.0 in
   /root/.cache/zlink/zlink-cpp/1.23.0 and Core /root/.cache/zlink/core/1.23.0/linux-x64), matching the
   binding benchmark's shape (window 100, 1024-byte payload). Confirm it reproduces the binding cell
   within 5%.
2. Add the Framework's real code (copied or linked from /root/worktree/zlink-3867s, not re-implemented)
   one layer per rung, in the order the request actually traverses, e.g.:
   envelope encode/decode -> operation registry pending entry + deadline index -> route selection ->
   owner/lane turn -> target receive mailbox + permit -> application executor -> typed decode/DI ->
   reply + completion callback -> full public API (= current Framework cell).
   Adjust order or split a layer if the code makes another order natural; say why.
3. For each rung, condition B (source CPUs 0-7, target 8-15), 30 s warm-up, 3 runs, median: msg/s and
   per-thread CPU µs/msg from /proc (same method as csubmit/ and cthin/; reuse their runner scripts).
   Report the delta per rung and which thread it lands on. Do send-saturation 1K too if time allows.
4. For the two or three rungs with the largest delta: perf the rung, name the functions responsible
   (file:function), the spec clause that requires the work (spec file:line under
   framework/doc/framework/common/spec/server/) or "no clause", and a concrete way to keep the function
   at lower cost, with an estimated µs saving.
5. Check the rung sum against the full Framework cell; explain any gap > 10%.

## Write limits and forbidden
/root/worktree/zlink-3867s is read-only for you. All code, builds, scripts and data live under
/root/jobs/fx/cx/v21/s033/cladder/. Forbidden: commit, push, branch, stash, reset, any repository edit,
spec edits, GitHub operations, git pull/fetch, docs sync scripts.
C++ builds: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 <cmd>`.
Measurements and perf: `flock -o /root/jobs/fx/cx/measure.lock ...`, one at a time. Keep MemAvailable
>= 12 GiB. Stop processes with SIGTERM/SIGINT only.

## Report
/root/jobs/fx/cx/v21/s033/cladder/report.md:
- rung table (rung, what was added, msg/s, F/B, per-thread µs, delta);
- the top-rung analysis from step 4;
- raw data paths;
- what could not be measured.
End your final message with:
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목"
(this job changes no product code, so 고친 항목 is none; list findings as 넘긴 항목).
