Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4352 Java L2 request-backpressure 1K — 5 more runs (verdict on hold). MEASUREMENT ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java-l2r, detached at bf83275257 (L2, production envelope encode path). Do not commit, push or open a PR. No code changes.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md and /root/jobs/fx/cx/v21/s033/lad-java-l2b/report.md.

## Why
lad-java-l2b measured L2 request-backpressure 1K 5 runs: L2 [142,235.4, 134,050.8, 136,888.0, 136,130.6, 112,731.0], G5 spread 17.2% (fail). Per the rule this cell is on hold; PM asked for 5 more runs.

## Do
1. Build in THIS worktree with framework/bench/grpc/java/build.sh; verify JARs newer than sources. Never use other worktrees' builds.
2. Copy /root/jobs/fx/cx/v21/s033/lad-java-l2b/tools to lad-java-l2r/tools, change only JOB/raw/worktree paths. Progress line before measuring.
3. One lock session: request-backpressure 1K, binding and L2, 5 runs. G5 spread = largest |run − median| ÷ median.
4. Report the new 5 runs alone, and all 10 L2 runs together (median, spread). Step loss = 1 − (L2÷binding)/1.007 (L1 ratio). Verdict: pass only if the new 5-run set passes G5 and loss ≤ 7%; if G5 fails again, say "판정 불가" and list the outlier runs with their timestamps.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-l2r/report.md (Korean, short table, numbers cite raw files). Time cap 2 h.
