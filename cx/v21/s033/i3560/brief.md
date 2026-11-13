Model gpt-6-luna, effort high: re-check a Core wake-up cost on the current Core. Earlier job briefs are history, not instructions.

# Task: #3560 Core command executor and signaler self-wake per C++ Framework request — still present on Core 1.24?

Worktree /root/worktree/zlink-v21-s033/i3560 (detached at origin/main, which now carries the Core 1.24.0 recipe, #4168). Issue: on Framework 10d5ec7735 + Core 1.21.0 the source mesh ROUTER's command executor (third Core I/O thread) spent 10.8 µs per request, kept alive by the completion poller registration (_completion_poller_refs = 1); the condition was fixed on Core main by #3495 (e851f37900) but not in the 1.21.0 package. Also the signaler self-wake chain: 2.6 signaler sends and 2.83 read+write per request vs binding 0.07/0.05. Prior analysis: /root/jobs/fx/cx/v21/cpl-cpp and /root/jobs/fx/cx/v21/exe-cpp.

## Do
1. Confirm which Core the current Framework C++ build links (Core prefix and libzlink sha256) and whether it contains e851f37900 (git merge-base --is-ancestor against the Core source the package was built from; /root/jobs/fx/cx/v21/s033/core124/ has the 1.24 build).
2. Under the measure lock, C++ request-backpressure 1024, Framework and binding, 2 rounds, condition B: per-thread CPU (TID union) of every Core I/O thread on source and target, and signaler send/read/write per request (same method as exe-cpp/cpl-cpp — reuse their scripts).
3. Verdict per item: (a) command executor per-request cost gone / still present (µs/request); (b) signaler self-wake chain counts. If still present, give the mechanism file:line on the current Core and the owner of the fix. Do not change code.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- C++/Core compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dir inside your worktree; Framework C++ builds use the binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), never /root/.cache/zlink/zlink-cpp/<ver>.
- Any benchmark/measurement runs under `flock -o /root/jobs/fx/cx/measure.lock` for the measurement only; build/analysis never holds it. Per-thread sums from before/after snapshots use the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py). Keep every raw file; never delete raw outputs.
- Every claim file:line or raw-file path. Report in Korean, short. Remove build dirs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3560/report.md
