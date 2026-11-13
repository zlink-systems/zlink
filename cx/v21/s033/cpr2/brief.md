# Task: C++ request path — turn the measured prototype (steps 1–5) into a clean PR-ready change (#3867)

## Situation
A measurement-only prototype (/root/jobs/fx/cx/v21/s033/cproto/: report.md, prototype.diff, wire_check.cpp and its logs) removed
per-request work the current contract allows: (1) prepared transport entry instead of two indexes and a lane round trip,
(2) TLS header-plan matching / static fragment selection moved to the bind/route plan, (3) the separate JSON header native message
and its copy into the service frame replaced by writing canonical header bytes once into the final service payload,
(4) the raw bool → host submit_result → mesh observer chain replaced by passing the final admitted source to the binding adapter,
(5) the public-host forwarding call removed. Result: request-serial 1KiB round trip 133.2 → 127.6 µs, backpressure caller about the
same (-0.44 µs). Unit+contract 297/297, wire bytes 12/12 identical. The PM approved making this a real PR, with line-by-line review.

## Do (worktree /root/worktree/zlink-3867e, branch perf/3867-cpp-request-encode-once, latest main 213e579f52)
1. Apply the product part of prototype.diff (`git apply --exclude='framework/bench/*'` already checks clean). Do not bring bench,
   run_local.sh or measurement helpers.
2. Review every hunk as production code: remove anything that exists only for the measurement (workarounds, probe-only exception
   captures that change production behaviour, dead helpers, duplicated legacy wrappers that now have no caller — delete them; keep
   wrappers other paths still use). Code must read like the surrounding code (naming, comment density).
3. Make the wire byte-identity check permanent: a unit or contract test that encodes the bench-like request (1 KiB payload, fixed
   correlation/deadline, the metadata/flow/topic/no-deadline cases from wire_check.cpp) and compares with fixture bytes captured from
   main's encoder (generate the fixture from the unmodified main code path, commit it as test data). Include a negative case.
4. Keep the native-call-count test expectations consistent with the removed header frame (init 8→7, close 10→9, copy 2) and explain.
5. Build all affected targets: `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0`
   (no outer flock). Run C++ unit + contract: `/root/jobs/fx/cx/measure-shared.sh ctest --preset linux-ninja-release -L '^(unit|contract)$' --output-on-failure`.
   Run `framework/languages/cpp` format check (the repo's format script, --check) and fix.
6. Do not measure throughput; the supervisor measures.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git commit/push/branch operations (git diff/status/apply only); no spec edits; no GitHub operations.
  Do not read Core C headers (core/include). Do not modify bindings/ or core/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- Decision owners stay one each (C1 terminal, deadline, physical admission/retry, typed delivery); no new flag/counter/queue/cache
  without a removal. Report before/after rule counts and the list of deleted mechanisms.
- 리팩토링 점검: list findings (performance, POSDDD, unnecessary code) and what you fixed.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/cpr2/report.md (Korean): changes per step with file:line, deleted mechanisms, tests added/changed
(and why each expectation changed), build/test/format results, remaining risks. Save the final diff to cpr2/final.diff.
