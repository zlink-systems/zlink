Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 .NET L0 fresh re-measure (receive parity) + #4351 ladder L1 (permit) — .NET. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-dotnet, branch exp/4350-dotnet-ladder, HEAD a542ee2384 (kairos committed the L0 receive-parity change: 100 ms blocking Recv, no DontWait drain). Commit L1 locally on top; never push, never open a PR.
Read first:
- /root/jobs/fx/cx/v21/s033/common-rules.md (rules for all ladder jobs; they apply here)
- /root/jobs/fx/cx/v21/s033/issue-4350.md and /root/jobs/fx/cx/v21/s033/issues-4351-4357.md section #4351 (permit acquisition before receive and its return just before the handler's first instruction; spec 01-execution/04-application-job-queue-and-backpressure.ko.md in the worktree)
- /root/jobs/fx/cx/v21/s033/lad-dotnet/report.md (L0 feature table with file:line) and /root/jobs/fx/cx/v21/s033/lad-dotnet-l0e/report.md (receive parity)
- For reference only: Java L1 restored permit at ZLinkFrameworkBenchServer.java:122-151 using ZLinkApplicationJobQueue.acquireBlocking(REMOTE) (/root/jobs/fx/cx/v21/s033/lad-java-l1/report.md).

## Why L0 is re-measured
Only request-backpressure 1K was measured after the parity change. The other five L0 cells were measured while L0 still drained with DontWait and raw did not, so they do not count.

## Do
1. Build a542ee2384 (Release). Measure L0 vs binding, 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K), 3 runs; request-serial 4K and any G5-failing cell get 5 runs. Record the receive/reply parity excerpts in progress.md before measuring.
2. Restore ONLY the permit feature on top of L0, using the real Framework .NET permit classes listed under "permit" in the L0 report. Everything else stays as L0, including receive and reply parity. Commit. Record file:line.
3. Measure L1 vs binding, same 6 cells and run counts, in its own lock session after a fresh build of the L1 commit (build && verify DLL newer than sources && measure).
4. Judge: L0 pass = L0÷binding ≥ 0.95 per cell. L1 step loss = 1 − (L1÷binding)/(L0÷binding) using the fresh L0 ratios from step 1. Pass if ≤ 7%. If a cell loses more than 7%, locate the cost (file:line plus allocation/lock count or profile) and write a proposal that rebuilds permit at binding-level cost; do not implement it.
5. No cross test this step.

## Output
/root/jobs/fx/cx/v21/s033/lad-dotnet-l1/report.md (Korean, short tables, each number cites its raw file):
- fresh L0 table (6 cells, medians, ratio, spread)
- L1 table (medians, L1÷binding, L0÷binding, step loss, judgement)
- restored code file:line and the commits
- if a cell fails: the cost location and the proposal
Time cap 4 h.
