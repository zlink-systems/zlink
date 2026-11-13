# Job (read-only measurement): where the per-request locks and heap calls come from — 0.33.0 #1639 / #3867

Model/level: gpt-6-luna medium — light tier: measurement and collection reusing an existing harness, no product change.
Worktree /root/worktree/zlink-3867h (HEAD 5ecae5de8b), read-only. No sub-agents or other models (no Astra, no Fable).
Do not run scripts/dev/internal-docs.sh, git pull/fetch or docs sync scripts. Do not read Core C headers. No GitHub.
No commit, push, branch, stash, reset, rebase, spec edits. Do not edit the worktree. Write only under
/root/jobs/fx/cx/v21/s033/csite/. Report: /root/jobs/fx/cx/v21/s033/csite/report.md.

## Background
/root/jobs/fx/cx/v21/s033/chand/report.md section 2 measured, per completed request on the source process
(request-backpressure 1 KB, condition B): heap calls caller 17.9 / receive role 9.2 / completion workers 7.0 / other 2.2;
explicit mutex acquisitions caller 36.6 / receive role 44.3 / workers 6.0 / other 9.4. Its counting build, run.py,
counters.cpp and check-counts.py are in that directory; reuse them (copy into csite/).

## Deliver
1. Same counts for the binding benchmark (zlink-cpp, same pattern, same condition, same counters, same run length),
   per thread role, so we have binding vs Framework per request.
2. Attribution for Framework: for mutex acquisitions and heap calls, sample or record the call stack (symbolized,
   enough frames to name the Framework/binding function that asked) and give the top call sites per thread role,
   per request, until they cover at least 80% of each count. Do the same for the target process
   (receive + workers + IO), since target CPU is also over budget.
3. One table: call site (function, file:line) | role | locks/request | heap/request | present in binding run?
   Keep sites that exist only in Framework separately from shared binding/Core sites.
Facts only; no design proposals beyond one line per site saying which Framework decision it serves.

## Limits
Builds of job-local copies: `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build ...`
(parallel 3). Measurement runs under `flock -o /root/jobs/fx/cx/measure.lock`, source CPUs 0-7, target 8-15,
30 s warmup / 5 s active is enough for counts. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; never use &.
Keep logs in files and read only summaries. End the report with
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".
