Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4351 ladder L1 (application job queue permit) — Java. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java, branch exp/4350-java-ladder (L0 is committed there; commit L1 locally on top; never push, never open a PR).
Read first:
- /root/jobs/fx/cx/v21/s033/issue-4350.md (common method, budget, measurement conditions)
- /root/jobs/fx/cx/v21/s033/issues-4351-4357.md, section #4351 (this step: restore permit acquisition before receive and its return just before the handler's first instruction; spec 01-execution/04-application-job-queue-and-backpressure.ko.md, read it in the worktree)
- /root/jobs/fx/cx/v21/s033/lad-java/report.md (Java L0: the per-feature table of what L0 turned off, with file:line, and the L0 medians)

## Do
1. Restore ONLY the permit feature on top of L0, in the Framework bench server and client paths L0 stripped, using the real Framework permit classes (the ones the L0 report lists under "permit"). Everything else stays as in L0. Reply parity stays: the L0/L1 reply must keep doing the binding server's reply work (new envelope part, protobuf re-encode, two parts); never reuse the request parts. Record the restored code with file:line.
2. Before EVERY measurement, write one line to /root/jobs/fx/cx/v21/s033/lad-java-l1/progress.md: built commit (`git rev-parse HEAD` + dirty flag), worktree base (`git merge-base HEAD origin/main`), binary/JAR paths under THIS worktree and their sha256, plus the binding vs L1 reply excerpts (file:line). Never use /root/zlink builds.
3. Measure L1 and binding in the same session with /root/jobs/fx/cx/v21/s033/lad-java/tools/run-group.sh (copy it to lad-java-l1/tools and change only JOB/raw paths): kairos server, source CPU 0-7, target CPU 8-15, warmup 30 s, active 5 s, 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K). 3 runs for every cell EXCEPT request-serial 4K, which gets 5 runs (PM 10:08: L0 spread exceeded 10%). Every run: errors 0 and submitted = completed = received; discard broken cells. No cross test this step.
4. Judge per cell (issue #4351 rule): step loss = 1 − (L1÷binding)/(L0÷binding), using the L0 ratios from lad-java/report.md. Pass if loss ≤ 7%. If any cell loses more than 7%, do NOT redesign silently: locate the cost (file:line, with an allocation/lock count or a profile) and write a proposal that rebuilds permit at binding-level cost, with file:line, in the report. Do not implement the proposal in this job.

## Rules
- Measurement only inside `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE, as run-group.sh does. Build first, then chain build && verify the built JAR is newer than the sources && measure. Never measure a stale build.
- Java builds normally, outside the measure lock. No C/C++ compiles are needed; if any are, only via /root/jobs/fx/cx/cpp-build-slot.sh, Release, -j3, at most 25 GB.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Raw outputs go in /root/jobs/fx/cx/v21/s033/lad-java-l1/raw; never delete raw files.
- Write progress.md as soon as the first L1 send-saturation 1K number exists, then continue.
- Time cap 4 h. Korean, short tables; every number cites its raw file.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-l1/report.md:
- L1 vs binding for all 6 cells (msg/s medians, L1÷binding, L0÷binding, step loss, ≤7% judgement)
- restored code locations (file:line) and the commit
- if a cell fails: the cost location and the rebuild proposal with file:line
