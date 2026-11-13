Model luna high: bounded static diagnosis (commit range, allocation-site attribution) with the logs already collected.

# Task: #4143 — why C++ Framework request_call_allocations fails on win-x64 only (static diagnosis, no builds)

Earlier briefs in other folders are history, not instructions for this task. Folder: JOB=/root/jobs/fx/cx/v21/s033/w4143. Worktree: /root/worktree/zlink-v21-s033/w4143 (current main; read only).

## Facts
- Test: framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_request_call_allocations.cpp. Linux passes; win-x64 (MSVC Release) fails.
- Nightly framework-cpp results: 11-04 999d67638f4a0c4182f8bd2743e14d1921dd6289 pass; 11-05 6f10186f97 fail at :411 (node request above transport, actual 11 vs encode+target_owner+receiver 10); 11-06 dc3a609aa9 fail at :221 (local Actor request completion registration total=10 > 8, sizes 1688,88,2192,240,312,152,64,128,64,88) and :739 (6 vs 5); 11-08 a306452334 and #4137 run (main 0005a4d6c0) fail at :222 with sizes 2280,88,2192,240,328,152,64,128,64,88. Logs (with allocation event lists and unsymbolized stacks): $JOB/r1105.log, r1106.log, r1108.log, r4137.log. Earlier: 11-03 nightly 2278cc19f4 also failed this test on win-x64 at a different assertion (:624, actual 22 not < 22) together with send_allocations, route_payload_ownership and connector_dispatch_allocations (log $JOB/r1103.log); 11-04 999d67638f passed; 11-01 failed for other reasons. So Windows allocation counts in these tests have broken before; say whether it is the same root cause (MSVC-specific allocations the Linux-derived expectations do not allow for).

## Do
1. List commits in 999d67638f..6f10186f97 and 6f10186f97..dc3a609aa9 (and later up to 0005a4d6c0) that touch the C++ request path (framework/languages/cpp runtime request/actor/completion code, the test itself, bindings/cpp used by it). For each, say what allocation it could add or remove.
2. Read the test: what it counts at :221/:222 (registration window), :409-411 and :737-739, and how the baseline 8 was chosen (git log -L or blame the expectation line; cite the commit and its message).
3. Attribute each allocation size in the Windows lists to a candidate site. Compare with what Linux allocates on the same path by reading the code (do not fetch more CI logs). Coroutine frames: MSVC does not elide coroutine frames where GCC may (HALO); sizes 2280/2192/1688/1552 are likely frames — identify which coroutine functions (by locals/awaiters size reasoning) and whether GCC elides them.
4. Decide: does the product allocate more on Windows (real cost, fixable: name the site and a fix), or is the expectation Linux-specific (state the evidence)? If a change is needed, describe it with file:line; do not edit files.
5. If symbolized stacks are needed to settle it, write exactly what CI change would produce them (e.g. PDB + symbol resolution in the allocation trace) and which run to dispatch; do not dispatch anything.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No builds, no tests, no measurements; do not touch /root/jobs/fx/cx/measure.lock.
- No git operations except log, show, diff, blame, status, rev-parse. No gh commands. No push, PR, comment, dispatch. No spec edits. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not read other job folders.
- 리팩토링 점검: note duplicated allocation sites (same object built twice on the path) with file:line.
- Keep your own output short. Time box about 3 hours.

## Report: $JOB/report.md (Korean, plain sentences): first failing commit and why; each Windows allocation → site; Linux vs Windows difference; verdict (product vs expectation) with evidence; proposed fix or CI symbolization step.
