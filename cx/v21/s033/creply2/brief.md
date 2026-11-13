Model gpt-6.1-sol, effort high: continue a concurrency change (C++ reply submitted outside the port mutex, close racing in-flight replies) after the previous session ran out of tokens.

# Task: finish the C++ reply unlock (#3867) with a deadline-based close retry

## Situation
Read /root/jobs/fx/cx/v21/s033/creply2/brief.md (the original task) and creply2/report.md (the previous session's draft). Worktree /root/worktree/zlink-3867-creply2 holds its uncommitted change (`git diff`). Reuse its tools/ and logs; do not rebuild what is already built unless sources changed.
The previous session copied a close retry of BUSY every 1 ms at most 8 times. That bound just failed Java CI under load (PR #4056): a reply that passed the closed check and was descheduled held the handle longer than 8 x 1 ms and close failed. The PM decision for Java, which C++ must follow:
- do not bound the BUSY retry by a fixed attempt count;
- bound it by the Framework's existing default termination/shutdown deadline (find the C++ equivalent of Java's ZLinkFrameworkRuntime.DEFAULT_TERMINATION_DEADLINE, 30 s, file:line); one deadline per close call shared by all sockets it closes; BUSY only; past the deadline the existing failure contract (failure returned, ownership kept);
- the deadline is a setting, not state: one constructor parameter with that default is allowed so tests use a short deadline (tens of ms) instead of waiting 30 s. No other new field, counter, lock or timer.
Race test results: replies racing a close may legitimately fail inside binding/Core with codes other than TERMINATED (Java saw NOT_FOUND and INVALID_HANDLE). Assert the Framework contract: close succeeds; replies after the port close are rejected by the existing null check; messages are consumed or released; a reply in the race window either succeeds or fails through the binding submit error. Record the observed binding result codes and their counts.

## Do
1. Replace the 8-attempt retry with the deadline policy above; keep it in the one shared close function.
2. Tests: keep the 100-repetition race test and the deterministic BUSY-once test; replace any 8-attempt test with a short injected deadline that expires while BUSY (failure returned, ownership kept, next close cleans up). Keep the negative control showing socket.cpp:53 leaves the native socket open without the explicit close.
3. Build only what the tests need first, then the full C++ ctest unit and contract suites; record logs. Run the race test 1000 times under stress-ng load (inside /root/jobs/fx/cx/measure-shared.sh, stop with SIGTERM) and record results.
4. Measure (original brief step 4): unpaced RUNS=5 request-backpressure 1KiB and 4KiB, request-serial 1KiB, main vs change, binding in the same session; reply packets/request and mutex wait at the paced rate.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except creply/ and creply2/; write only in creply2/ and this worktree.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify bindings/ or core/.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare. Build with enough parallelism to finish in reasonable time when memory allows (MemAvailable >= 12 GiB); do not hold the build slot for a full-suite build at -j1. Other builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock`, condition B (source CPUs 0-7, target 8-15).
- SIGTERM/SIGINT only; no background jobs with &. Keep your own output short: summarise logs instead of printing them.
- Framework÷binding = Framework throughput ÷ binding throughput. Report "over budget by X µs" per request (budget = binding µs / 0.6). Do not suggest lowering the 0.6 target.
- Time box about 5 hours; waiting for the measurement lock does not count.

## Output: overwrite /root/jobs/fx/cx/v21/s033/creply2/report.md (Korean, plain sentences) as the final report: close sites and safety, deadline source file:line, tests with log paths and the 1000-run results with observed codes, RUNS=5 table, decision-site count. Save creply2/change.diff (framework/ only) and bench edits separately.
