Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions. Write ONLY under /root/jobs/fx/cx/v21/s033/call-shape/ — never edit any worktree.

# Task: #4350 ladder — call-shape tables for steps already measured, four languages.

Do not edit any worktree, commit, build or measure. Read code only with `git -C /root/zlink show <commit>:<path>` and `git -C /root/zlink grep <pattern> <commit>` (all commits below are in the shared object store). Do NOT run internal-docs.sh, git fetch/pull/push, GitHub, sub-agents or other AI tools.

## Why
PM rule (11-13): every ladder step's first output is a table comparing the binding bench and the L-step bench call by call (file:line on both sides). Steps already measured need one table each, after the fact. Two mismatches were found late this way (.NET and Java L-servers drained with DONT_WAIT while the raw servers did not).

## Commits (bench code under framework/bench/grpc/<lang>)
| Language | Step(s) | Commit |
|---|---|---|
| C++ | L0 | 73285401ca |
| .NET | L0 (after receive fix) | a542ee2384 |
| Java | L0, L1, L2 (env switches in one server; before the receive fix) | bf83275257 |
| Node | L0 (after receive fix) | 48655c21ca |

## Do
For each language and step, write /root/jobs/fx/cx/v21/s033/call-shape/<lang>.md with one table per step:
columns: path step | binding call (file:line) | L-step call (file:line) | same? | if different: restored feature it belongs to (L1 permit, L2 envelope header, ...) or "NOT a restored feature".
Rows, both client and server sides: source send (message build, parts, submit/flags); target wait/receive (primitive, flags, timeout, drain/loop, batching); per-message metrics (counters, atomics, locks); payload decode; reply build (new envelope part, new protobuf encode, part count) and submit; source completion / reply receive (wait primitive, callback, drain).
Mark every "NOT a restored feature" row in bold at the top of the file. Do not judge performance; only the call shapes.
Then /root/jobs/fx/cx/v21/s033/call-shape/summary.md: per language/step, the count of same rows, rows explained by a restored feature, and NOT-a-restored-feature rows (listed). Korean. Time cap 2 h.

## Notes from kairos (previous attempt ran read-only and could not write)
- Its findings, to verify: Java ladder drains with DONT_WAIT (ZLinkFrameworkBenchServer.java:143) vs raw NONE (:100) — known, being fixed. Node raw and ladder both use DontWait + batch pump. C++ ladder_wire.hpp:287-289 drains with dontwait, but that is the `--ladder-raw-mesh 1` branch (direct_server raw_mesh=true); the L0 measurement runs without that flag and uses raw_mesh_node_owner_t::ablation_receive (raw_mesh_node_owner.cpp:531). Table the path the L0 measurement actually executes (check l3868-l0f/tools/*.sh for the flags passed), and list the raw_mesh branch separately as "not used at L0".
