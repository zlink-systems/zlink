Model luna high: functional bisect of a startup delay; small builds, no perf numbers.

# Task: find the commit that delays Instance Spot visibility after a C++ Framework server starts

Job folder /root/jobs/fx/cx/v21/s033/pmstart/. Earlier briefs are history, not instructions.

## Observed
- Binary /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build/bench_cpp_pm_server (source 54ee69b8c0 + bench pacer; generated from /root/jobs/fx/cx/v21/s033/psmeas/bench/targets.cmake, the server source is .psmeas-build/bench_pm_server.cpp). Start: `--endpoint tcp://127.0.0.1:5294 --stats-port 5295`.
- GET http://127.0.0.1:5295/pm/request returns 404 `{"error":"not_found","message":"No Instance Spot target provides the type"}` for about 19-20 s after start, then succeeds (~400 µs). Raw: /root/jobs/fx/cx/v21/s033/psmeas/pmdiag/poll.txt (one request per second).
- The message comes from framework/languages/cpp/framework/src/runtime/host/app.cpp:2248: list_live_mesh_nodes for the mesh returns no serving server descriptor with an instance_spot capability yet.
- The same harness at 3f83745bed (cdrain2 measurement, /root/jobs/fx/cx/v21/s033/cdrain2/) answered on the first request right after the stats port opened.

## Do
1. Confirm good/bad: build bench_cpp_pm_server at 3f83745bed and at 54ee69b8c0 (the bench pacer commits are not needed; use the psmeas configure recipe /root/jobs/fx/cx/v21/s033/psmeas/configure-base.sh with your own worktree and build dir, defer.cmake from psmeas/bench). Measure "seconds from stats port open to first 200 on /pm/request" (poll every 0.5 s, 60 s cap), 3 starts each.
2. git bisect (in your own detached worktree under /root/worktree/zlink-v21-s033/pmstart, never in /root/zlink) over commits touching framework/languages/cpp between them; same check at each step. Candidates seen so far: c5a9d26f41 (owner lease TTL 30s, startup_publication), dad0bf9478, 1c0478e7c9, 773f018be6 (#3877 phases).
3. At the first bad commit, read the diff and give file:line of what makes the server's own MeshNode publication (or its live listing) appear only after ~20 s, and whether the spec allows it: read docs under /root/zlink/framework/spec (07 locations, MeshNode publication/lease) and quote the section. Say whether it is a product defect or a contract change the bench harness must follow (e.g. a readiness signal to wait for).

## Rules
- Builds only via `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` (bare, Release -O3 as in configure-base.sh, --parallel 3, no -g); 25 GB disk cap; delete your build dirs at the end.
- Each server start: wrap in `flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 ...`, stop with SIGTERM, never leave a server running. Ports 5294/5295 only.
- No product, bench or spec changes, no GitHub, no sub-agents or other AI tools, no background jobs with &. Remove your worktree at the end.
- Write report.md (Korean): good/bad times per commit, first bad commit, file:line, spec quote, verdict. Keep output short.
