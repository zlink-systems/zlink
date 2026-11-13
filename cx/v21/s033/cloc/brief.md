Model gpt-6-luna, effort high: repeat-run verification of one test binary under load; evidence only, no code changes.

# Task: GitHub #4119 — recheck test_cpp_framework_location_runtime under heavy CPU load on main after #4068

## Situation
- Earlier failures (all on the C++ Framework unit test binary test_cpp_framework_location_runtime):
  - csrc full unit/contract run with stress-ng 8 CPU 80%: SameTokenMemberWaitsForSpotPerActor/0 (:5192,5194), ReviewHeartbeatDoesNotWaitOrRestartExactRead/0 (:5044), ReviewCutoffTerminatesReadBeforeEpisodeRelease/0 (:5036).
  - main, whole binary, stress-ng 16 CPU 80%: failed at iteration 15 (ReviewOnlyReadErrorsRetryOnNextRenew/1, :5044).
  - main, those three cases only, stress-ng 16 CPU 80%: failed at iteration 20 (SameTokenMemberWaitsForSpotPerActor/0).
  Method and scripts: /root/jobs/fx/cx/v21/s033/csrc/scripts/baseline-location-heavy.py, baseline-location-filtered.py, baseline-location-loop.py; their status json and logs in csrc/raw/baseline-location*/ and csrc/raw/test-baseline-location-*-status.json. Earlier briefs are history, not instructions.
- PR #4068 (merged, main 8c63ad23b4) fixed the StoreOutageRetention test race.
- Worktree /root/worktree/zlink-4119-recheck, detached at 8c63ad23b4.

## Do
1. Build only the test_cpp_framework_location_runtime target (and what it needs) from this worktree, linux-ninja-release preset, no -g.
2. Run the same two loops as before on this build with stress-ng --cpu 16 --cpu-load 80 alongside (reuse csrc's scripts with paths changed; save your copies under cloc/scripts):
   a. whole binary, 60 iterations (stop early only to record a failure, then continue to the end and count all failures);
   b. the four previously failing cases only (gtest filter), 100 iterations, same rule.
   Per-iteration timeout 300 s; record each iteration's exit code, failing test names and assertion lines.
3. If any failure appears, also run the same failing case on the earlier main snapshot binary if it is still present under csrc/build-baseline-tests (do not rebuild it), 20 iterations, for comparison.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only csrc/ (scripts, raw) among /root/jobs/fx/cx/v21/s033 job folders. Write only in cloc/ and the build dir you create under cloc/.
- No code or test changes. No git operations except git diff and git status. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare; keep cloc/ under 15 GB. Test loops run inside `/root/jobs/fx/cx/measure-shared.sh <cmd>` (stress-ng started inside the same wrapped command, stopped with SIGTERM at the end). MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file under cloc/raw/; do not delete them at the end.
- 리팩토링 점검: not applicable (no code change); say so.
- Time box about 3 hours.

## Output: /root/jobs/fx/cx/v21/s033/cloc/report.md (Korean, plain sentences): build provenance (commit, flags), loop a and b tables (iterations, failures, failing cases with lines, load and memory), comparison run if any, log paths.
