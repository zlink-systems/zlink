Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 Node L0 request-serial 4K (0.916) — find the difference. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-node, branch exp/4350-node-ladder, HEAD 48655c21ca. Commit locally; never push, never open a PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (applies here) and /root/jobs/fx/cx/v21/s033/lad-node-l0e/report.md.

## Known
- L0 serial 4K = 8,009.6 vs binding 8,748.6 msg/s (0.916; aggregator G5 passed). Mean latency 0.122 vs 0.111 ms. Other five cells pass.
- Per-message code is nearly the same (zlink-framework-server/main.js:59-95 vs zlink-raw-server/main.js:47-142). Remaining differences kairos sees:
  a. L0 receives requests and commands on ONE router; raw uses two routers (request + command) and pumps both.
  b. The L0 process `require`s NestJS, @zlink-systems/nestjs, the Framework and the protobuf codec (main.js:13-27) even though the L0 branch never boots Nest. Target memory 137 MB vs 78 MB in the 4K cell.
  c. The L0 server keeps one `zlink.Received` and reads `received.parts` per message (same as raw's bodyPart) plus the `frameworkWire` branch.
- The previous profile covered only L0, so it cannot show where the two servers differ.

## Do
1. One lock session: paired CPU profiles (--cpu-prof, same method as lad-node-l0e raw/profile) of raw and L0 target servers at request-serial 4K, 1 run each. Compare self time per frame between the two; table the frames that differ by more than 1% of samples.
2. Build diagnostic variants of the L0 server, each a separate commit or env switch, changing ONE difference at a time: (a) two routers like raw; (b) no Nest/Framework/codec requires on the L0 path (lazy-require only in the non-L0 branch). Keep receive/reply parity rules.
3. One lock session: request-serial 4K × 5 runs for binding, current L0, L0+a, L0+b (and L0+a+b if both help). Progress line before measuring.
4. If a variant reaches ≥ 0.95 and is a legitimate L0 shape (the L0 definition: Framework path stripped to the binding shape), make it the L0 server, commit, and re-measure all 6 cells × 3 runs (G5 failures → 5 runs) against binding in one lock session. If none does, report the remaining per-frame difference with file:line.
- G5 spread = largest |run − median| ÷ median (benchagg spread_percent); fail above 10%.

## Output
/root/jobs/fx/cx/v21/s033/lad-node-l0f/report.md (Korean, short tables, each number cites its raw file): profile difference table, variant table (5-run medians, ratio, spread), the chosen L0 commit, and the 6-cell table if re-measured. Time cap 4 h.
