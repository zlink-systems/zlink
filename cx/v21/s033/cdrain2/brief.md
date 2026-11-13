# Job: C++ gate-less drain — scale out only when backlog grows, rotate helpers (#1639)

Model/level: gpt-6.1-sol high — scheduler/wake/concurrency path across executor and mailbox; a previous
attempt measured a regression, so this is a second pass with a supervisor-decided design.
No sub-agents or other models (no Astra).

## Goal
The committed drain (HEAD 5a585edc93) lets several workers take records of one gate-less owner. Result
(/root/jobs/fx/cx/v21/s033/cdrain/report.md): heavy 100 µs handlers 6.4x faster, but
- cheap 1K cells regressed: request-backpressure 0.1973 -> 0.1898, send-saturation 0.3006 -> 0.2572;
  application CPU 18.9 -> 49.9 µs/msg, target switches 1.39 -> 2.60/msg — every briefly-queued record
  wakes another worker;
- same-host Spot request p99 0.50 -> 7.59 ms while the hot owner holds all 8 workers.
Done when, with the same measurement scripts and conditions as that report (3 runs, condition B):
1. request-backpressure 1K and send-saturation 1K Framework medians are no lower than main minus 1%;
2. PM cell hot throughput is at least 4x main;
3. PM cell Spot p99 is at most 2x main (main ~0.50 ms).

## Location and write limits
Worktree: /root/worktree/zlink-1639c (branch perf/1639-cpp-multi-drain). Write only under
framework/languages/cpp/framework/src/runtime/{dispatch,mesh,channels,stateful}/ and
framework/languages/cpp/tests/. Read anything else.
Job files (scripts, logs, report): /root/jobs/fx/cx/v21/s033/cdrain2/ only. Report:
/root/jobs/fx/cx/v21/s033/cdrain2/report.md. Reuse scripts from /root/jobs/fx/cx/v21/s033/cdrain/.

## Forbidden
commit, push, branch, stash, reset, rebase; edits outside the write limits; spec/doc edits; GitHub
issue/board/PR operations. Leave changes uncommitted; the supervisor commits after review.
C++ builds only via `/root/jobs/fx/cx/cpp-build-slot.sh --parallel 3 <cmd>`; builds/tests under
`flock -s /root/jobs/fx/cx/measure.lock`, measurements under `flock -o` on the same file.

## Basis (spec root framework/doc/framework/common/spec/server/)
- 01-execution/04-application-job-queue-and-backpressure.ko.md "준비된 owner 집합": an additional worker
  *requires* untaken records and an idle worker (necessary, not sufficient); never notify unconditionally
  per record; no stranded work.
- 01-execution/02-handler-turn-and-execution-gate.ko.md §9 and 00-foundation/06-framework-api.ko.md §11:
  the per-worker budget is an upper bound; a worker gives another waiting ready owner a chance.
- 01-execution/08-messaging-hot-path.ko.md §4.3 W5.

## Decided design (do not substitute another)
1. Scale-out on backlog growth, no new time constant. A worker draining a gate-less owner remembers
   the owner's untaken-record count right after its take. At its next W5 boundary it wakes one more
   idle worker only if untaken records remain AND the count did not decrease since its take (arrivals
   kept up with or outran this worker). Otherwise it continues alone. The first worker for a newly
   ready owner is still woken by the existing publish path. Keep the decision inside the existing
   mailbox/executor state; no new flag, queue or counter beyond the remembered count (a local).
2. Helpers rotate. While a gate-less owner is served by more than one worker, each of those workers at
   W5 lets a different waiting ready owner run next before continuing this owner (rotate the existing
   executor queue as the budget path already does). A sole worker keeps the current budget rule.
3. Remove whatever the old unconditional extra-wake path becomes redundant; report the decision-location
   table before/after (it was 13 -> 6; it must not grow).
4. Tests: cheap synchronous records with an idle pool wake no extra worker (count wakes); records whose
   handler blocks on a test barrier while more arrive do scale out; a second ready owner runs between
   records of a multi-worker gate-less owner; all existing gate-less tests stay green.

## Verification
- Full C++ framework non-install tests (same set as the previous report) and `scripts/format/format.sh
  --check cpp`; logs in the job dir; read only first failure + summary.
- Measurements as in the previous report; put raw cells under cdrain2/measure/.

## Report
Result per done criterion (met/unmet + numbers), changed files and purpose, decision-location table
before/after, verification commands + log paths + summary lines, the line
"리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목", and remaining problems.
If a criterion cannot be met with this design, stop and report the data instead of inventing another rule.
