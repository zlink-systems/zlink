# Task: C++ request-backpressure 1KiB — split the source caller thread's 17.5 µs per request (read-only profile)

## Situation
C++ Framework request-backpressure 1KiB (tree of PR #3957: /root/worktree/zlink-3867h, HEAD 00f9963cc9) runs 51,942 req/s vs
binding 253,036 (0.205). The source caller (submit) thread uses 0.908 cores = 17.48 µs per request (ratio044cpp results.json
submit_thread_cores). For 0.6 it must be 6.59 µs. The PM needs the 17.5 µs split into: encode (payload/protobuf/envelope/service
wire), registration (operation owner turn, registry, deadline, admission), result observation (task/receiver/notification,
benchmark collect/validate), binding submit (binding request call + Core call + kernel send if on this thread), other —
plus the top 15 functions (self and inclusive µs/request, file:function) on the caller thread. Same for the binding caller (≈3.7 µs)
for comparison. Then: how much each part must shrink to reach 6.59 µs.

## Method (reuse, do not reinvent)
/root/jobs/fx/cx/v21/s033/cprof/ did this on an older main: read brief.md, report.md, build-helper.sh, collect.py, analyze.py,
candidates.py. Reuse its frame-pointer build approach, collectors and stage mapping; update the mapping for #3957 code.
Build the instrumented/frame-pointer bench from /root/worktree/zlink-3867h into THIS job directory only (do not touch that
worktree's files). Builds: under `flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh ... --parallel 3`.
Every bench/profile run: `flock -o /root/jobs/fx/cx/measure.lock ...`, one at a time, condition B (server/target CPUs 8-15,
source/client CPUs 0-7), request-backpressure, PAYLOAD_SIZES=1024, RUNS=1, SKIP_BUILD=1, outputs under this job dir. Profile
both Framework and binding. Also one un-profiled run of your build per implementation; report its req/s next to 51,942 / 253,036
so the build's representativeness is visible.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only for the repository: do not modify any worktree or tracked file; no git operations; no spec edits; no GitHub operations.
  Do not read Core C headers (core/include).
- 리팩토링 점검: not applicable (no product code change); write "코드 변경 없음".
- MemAvailable >= 12 GiB; delete raw profile files over 1 GB after summarizing. SIGTERM/SIGINT only; no background jobs with &.
- Time box about 90 minutes; write the report even if an item is missing and mark it "측정 못 함".

## Output: /root/jobs/fx/cx/v21/s033/ccall/report.md
Korean, plain sentences. Start with a table: part, Framework µs/request, binding µs/request, reduction needed toward 6.59 µs.
Then the top 15 Framework caller functions, then what the profile says about where a redesign must cut (no design proposal beyond one line per part).
