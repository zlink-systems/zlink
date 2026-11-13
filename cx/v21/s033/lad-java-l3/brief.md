Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4353 ladder L3 (service wire multipart framing) — Java. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java, branch exp/4350-java-ladder, HEAD bf83275257 (L2 with the production envelope encode path). Commit L3 locally on top; never push, never open a PR.
Read first:
- /root/jobs/fx/cx/v21/s033/common-rules.md (applies here)
- /root/jobs/fx/cx/v21/s033/issue-4350.md and issues-4351-4357.md section #4353
- /root/jobs/fx/cx/v21/s033/lad-java/report.md (L0 feature table: what "service wire" L0 turned off, with file:line), lad-java-l2/report.md and lad-java-l2b/report.md
- Spec in the worktree: framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md (and the service wire / multipart sections it links)

## Do
1. Restore ONLY the service wire multipart framing on top of L2, in the client and server paths L0 stripped, using the same Framework classes and call shapes the production send/receive path uses (trace production with file:line, as lad-java-l2b did for the envelope; never a slower or cheaper bench-only variant). Permit and envelope header stay as in L2; receive and reply parity stay. Commit; record file:line.
2. Fresh build (build && JAR newer than sources && measure). L3 vs binding, 6 cells × 3 runs in one lock session; request-serial 4K and any G5-failing cell → 5 runs. G5 spread = largest |run − median| ÷ median. Progress line before every measurement.
3. Step loss = 1 − (L3÷binding)/(L2÷binding). L2 ratios: request-backpressure 1K 0.957, send-saturation 1K 0.986, request-backpressure 4K 0.956 (lad-java-l2b); send-saturation 4K 0.973, request-serial 1K 0.981, request-serial 4K 0.967 (lad-java-l2). Pass ≤ 7%.
4. A cell losing more than 7%: paired JFR (same method as lad-java-l2/raw/profile/reqbp-1K), the cost with file:line, and a proposal that rebuilds it at binding-level cost. First check the bench uses the production call shape; if not, fix the bench and re-measure that cell instead of proposing a product change. Do not implement product changes.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-l3/report.md (Korean, short tables, every number cites its raw file): restored code file:line + the production path it matches, 6-cell table (medians, L3÷binding, L2÷binding, step loss, G5), proposal if any. Time cap 4 h.
