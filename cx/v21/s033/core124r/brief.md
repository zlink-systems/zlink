Model luna high: write the report for a finished release-gate run from existing files; no measurement.

# Task: Core 1.24.0 release gate — write report.md and verdict.md (step 6 and 7 of the gate brief)

The gate job folder is JOB=/root/jobs/fx/cx/v21/s033/core124b. Read $JOB/brief.md: do only its steps 6 and 7, and follow its Rules section. Steps 1–5 are done; the job that ran them stopped before step 6. Earlier briefs are history, not instructions.

## Inputs (all under $JOB)
- build-versions.txt, harness-verification.tsv, build-preflight.jsonl, preflight.jsonl: commits, zlink_version strings, SHA-256, harness SHA match.
- Runs-3 comparisons: gate-{C-vs-B1,C-vs-B2,B2-vs-B1}-{single,multi}-corrected.txt are the ones to quote (the files without -corrected are earlier runs of the same comparison; say in one line what "corrected" fixed, from codex.log or the files themselves).
- Runs-5 confirmation: confirm/groups.tsv (which groups were rerun), confirm/{B1,C,B2}-<suite>-<pattern>-<transport>.txt raw results, confirm/gate-<suite>-<pattern>-<transport>.txt re-judgements, confirm/times.tsv stage times. One group has *.stopped files from a clean stop and resume: use the completed rerun and say so. confirm/pause-1639io.txt lists two pauses where another measurement ran between groups; state which groups were measured right after each pause.
- Stage start/end times: logs/, times files, file mtimes.
- JOB0=/root/jobs/fx/cx/v21/s033/core124 (B1 and C, hotpath gate and wake invariant results; read only).

## Output
- $JOB/report.md then $JOB/verdict.md, Korean, exactly the content listed in step 6 (the failing table must give for every cell/aggregate still failing after runs-5: pattern, transport, size, metric, ratio runs-3 vs B1, runs-3 vs B2, runs-5, the same cell's A/A ratio B2 vs B1, and #4135/#3930 path). Add a column "A/A 안쪽?" saying whether the candidate ratio is within the A/A spread for that cell. Every number must come from a file; give the file path per table (not per cell).
- Also the 리팩토링 점검 list from the Rules.
- Step 7 after verdict.md: grep CMakeCache.txt files under /root/worktree and /root/jobs for "core124b/src-1.2"; if none, `git -C /root/zlink worktree remove` both src worktrees (use --force only if the only changes are build directories; say so). Do not touch any other worktree.

## Rules
No measurement, no build, no product/bench/spec changes, no GitHub, no sub-agents or other AI tools. Never delete raw outputs. Keep output short; finish with a 5-line summary: verdict, failing group count, groups within A/A, worktree removal result.
