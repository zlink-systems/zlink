Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4352 ladder L2 (envelope header) — Java. Worktree /root/worktree/zlink-v21-s033/lad-java, branch exp/4350-java-ladder (L1 committed at 15c8bab1a6; commit L2 on top).
Read first: /root/jobs/fx/cx/v21/s033/issue-4350.md (method, budget); /root/jobs/fx/cx/v21/s033/issues-4351-4357.md section #4352 (restore envelope header encode and parse; the header is parsed once and the payload only changes ownership, #1639 plan item 2); /root/jobs/fx/cx/v21/s033/lad-java/report.md (L0 table of turned-off items, "envelope header" row) and /root/jobs/fx/cx/v21/s033/lad-java-l1/report.md (L1 results); /root/jobs/fx/cx/v21/s033/common-rules.md (binding).

## Do
1. On top of L1, restore ONLY the real Framework envelope header: source-side encode in the Framework bench client path and target-side parse in the server path, using the real Framework envelope classes the L0 report lists. Permit stays on; everything else stays as in L1. The wire header bytes then differ from the binding header; that is the restored feature. Record the restored code with file:line. Check that the header is parsed once and that the payload is not copied because of the header; if it is parsed twice or the payload is copied, record that with file:line.
2. Measure L2 and binding in the same session, 6 cells, 3 runs (5 for any G5 failure). No cross test.
3. Judge each cell: step loss = 1 − (L2÷binding)/(L1÷binding), with the L1 ratios from lad-java-l1/report.md. Pass if ≤ 7%. If a cell loses more, do not redesign: locate the cost (file:line plus an allocation/lock count or profile) and write a proposal that rebuilds the envelope header at binding-level cost, with file:line, in the report.
Output dir /root/jobs/fx/cx/v21/s033/lad-java-l2 (progress.md, report.md, raw/, tools copied from ../lad-java-l1/tools). Time cap 4 h.
