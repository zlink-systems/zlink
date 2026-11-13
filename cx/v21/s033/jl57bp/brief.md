Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: Java ladder L5~L7 request-backpressure produces errors (capacity unavailable + timeouts) through the public Framework route. Decide whether this is a Framework defect or a bench configuration fault; report with file:line and a reproducer. No production fix in this job.

Worktree /root/worktree/zlink-v21-s033/lad-java-l57p, branch exp/4350-java-ladder-l57p, commit ee7e5c3343 (built installs listed in lad-java-l57p/report.md). No other job uses this worktree now. Commit nothing unless step 3 applies; never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Never delete raw files.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (incl. LOAD TESTS DURING OTHER HOLDS), ladder-steps.md, lad-java-impl5/report.md + call-shape-L5-7.md, lad-java-s2/report.md and lad-java-s3/report.md (the failing raw results.json and scheduling.txt are linked there).

## Evidence
- lad-java-s2 (old build): L5~L7 request-backpressure 4K counted errors 123,625 (warm 128,213); submitted/completed/received differ.
- lad-java-s3 (protobuf registration off): L5~L7 request-backpressure 1K warm errors 50,160 = capacity unavailable 20,118 + timeout 30,042; submitted 204,305, completed 154,145, received 184,187.
- Binding and L0~L4 request-backpressure runs had errors 0. send-saturation and request-serial at L5~L7 had errors 0.
Questions to answer with evidence: what raises "capacity unavailable" (file:line) and which limit it enforces (owner queue capacity, operation registry size, in-flight limit, admission window) and its configured value in the bench; does the bench client keep more requests in flight than that limit while the binding/L4 path has no such limit (i.e. the request-backpressure pattern's window vs the Framework limit); why requests time out (deadline value vs queueing time); does the main (non-ladder) Framework Java bench show the same errors at request-backpressure 1K/4K? What does the spec say the caller sees under backpressure (spec file:line) — is an error the specified behaviour, or should admission wait?

## Do
1. Reproduce L5~L7 request-backpressure 1K and 4K plus the main Framework bench at the same cells, each short run inside your own exclusive hold (flock -x /root/jobs/fx/cx/measure.lock, at most 10 minutes per hold), pin source 0-5 / target 8-13. Capture error kinds, in-flight counts, queue depths if exposed.
2. Write /root/jobs/fx/cx/v21/s033/jl57bp/report.md: cause with production file:line, spec reference, verdict (Framework defect vs bench configuration vs specified behaviour), exact reproduce commands, raw under jl57bp/raw.
3. Only if it is a bench configuration fault that diverges from how the main Framework bench is configured: fix it in one commit on this branch, rebuild here, and show one short run per backpressure cell with errors 0. Otherwise change nothing.
Time cap 2 h.
