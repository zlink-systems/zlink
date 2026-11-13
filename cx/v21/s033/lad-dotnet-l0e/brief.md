Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: close #4350 ladder L0 — .NET. Worktree /root/worktree/zlink-v21-s033/lad-dotnet, branch exp/4350-dotnet-ladder.
Read first: /root/jobs/fx/cx/v21/s033/issue-4350.md; the previous report /root/jobs/fx/cx/v21/s033/lad-dotnet/report.md; /root/jobs/fx/cx/v21/s033/common-rules.md (binding).

## Do
1. Check receive parity against the raw server (ZLinkRawServer/Program.cs) per the rules and record it. Fix only if the L0 receive differs beyond the one direct fixed-handler call.
2. request-backpressure 1K was 0.946 with equal allocation (1456 B/receive both). Re-measure that cell only, L0 and binding in the same session, 5 runs, and judge with the 5-run median.
3. The Framework-server + Framework-client cross test failed because the real Framework client sent route-mesh frames to the L0 server ("invalid tag (zero)"). At L0 that combination means the Framework bench client executable in L0 mode, whose operation is the binding-shaped transport (Java did this: FrameworkStack in L0 mode uses RawStack). Add that L0 client mode, then run the 3-way cross test at send-saturation 1K (raw/raw, L0 server + raw client, L0 server + L0 client), 3 runs.
Output dir /root/jobs/fx/cx/v21/s033/lad-dotnet-l0e (progress.md, report.md, raw/, tools copied from ../lad-dotnet/tools). Keep the old raw. Time cap 3 h.
