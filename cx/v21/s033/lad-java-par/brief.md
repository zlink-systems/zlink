Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350–#4353 Java ladder — fix receive parity, re-measure L0/L1/L2, then finish L3. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java, branch exp/4350-java-ladder, HEAD c20022b27e. That top commit is an UNFINISHED L3 (service wire) WIP from a stopped job; L2 is bf83275257. Commit locally; never push, never open a PR.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (applies here), lad-java/report.md, lad-java-l1/report.md, lad-java-l2b/report.md, lad-java-l2r/report.md, issues-4351-4357.md section #4353.

## Why
lad-java-l2r found the Java ladder server breaks receive parity: ZLinkFrameworkBenchServer.java pumpLadderL0 (:123-160) does one blocking recv and then drains with RecvFlags.DONT_WAIT; the raw server (ZLinkRawBenchServer.java:94-109) does one blocking recv per message, no drain. This has been there since L0 (5acfffcd02), so the Java L0, L1 and L2 ratios were all measured with a receive the binding does not have. .NET had the same drain and its cells are being re-measured; Java must be too.

## Do
1. Receive parity: change pumpLadderL0 to the raw server's loop — one blocking `recv(received, NONE)` per message, no DONT_WAIT drain. Keep the L1 permit where the L1 spec puts it (acquire before each receive, return just before the handler's first instruction); the permit is the only allowed L1 difference. Commit this on top of the WIP as its own commit (it applies to L0/L1/L2/L3 via their env switches). Record raw vs ladder receive excerpts side by side in progress.md.
2. Finish L3 from the WIP: restore ONLY service wire multipart framing on top of L2, using the production send/receive call shapes (trace them with file:line, as lad-java-l2b did for the envelope). Commit.
3. Fresh build (build && JARs newer than sources). Then measure in as few lock sessions as possible: binding, L0, L1, L2, L3 — 6 cells (send-saturation, request-serial, request-backpressure × 1K, 4K) × 3 runs each; request-serial 4K and any G5-failing cell → 5 runs. G5 spread = largest |run − median| ÷ median; a cell still failing G5 after 5 runs is "판정 불가", never a pass. Progress line before every measurement.
4. Judge: L0 ≥ 0.95 × binding per cell. Step loss Ln = 1 − (Ln÷binding)/(Ln−1÷binding), using the NEW ratios from this job. Pass ≤ 7%. A cell over 7%: first confirm the bench uses the production call shape (fix the bench and re-measure if not); otherwise paired JFR, the cost with file:line and a binding-cost proposal; do not implement product changes.
5. Compare each new ratio with the old (drain) ratio in one column so the drain effect is visible.

## Output
/root/jobs/fx/cx/v21/s033/lad-java-par/report.md (Korean, short tables, numbers cite raw files): parity change file:line + commit; L3 restored code file:line + production path; one table per step (L0..L3: medians, ratio, old ratio, step loss, G5, verdict). Time cap 6 h.
