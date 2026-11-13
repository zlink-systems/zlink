Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: .NET ladder L4 — use the DEFAULT typed JSON serializer as #4354 requires, then replay L5~L7. IMPLEMENTATION/TESTS ONLY, NO MEASUREMENT. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-dotnet, branch exp/4350-dotnet-ladder, tip 2b6da5af64 (L2 69891f4446, scheduler capture bdb801fd2e, L3 e8ee8302b0, L4 1bcaebde9e, L5~L7 2b6da5af64; lad-dotnet-l2g/report.md). Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Do NOT take /root/jobs/fx/cx/measure.lock, run no benchmark or load. Never delete raw files. Read /root/jobs/fx/cx/v21/s033/common-rules.md, ladder-steps.md and the #4354 section of issues-4351-4357.md first.

## Review finding
#4354: "기본 typed JSON serializer 경로를 씁니다. 메시지별 codec 등록이나 호출부의 수동 encode/decode로 비용을 맞추지 않습니다." The .NET L4 commit 1bcaebde9e registers ZLinkProtobufCodec (CreateLadderCodecs → codecs.Use(ZLinkProtobufCodec.Default)) instead. C++ (276b84f248) and Java (9d9901eb2c) use the default typed JSON serializer with an ordinary DTO; for reference read them with git show (do not touch their worktrees).
1. Change L4 to the production DEFAULT typed JSON serializer path for an ordinary DTO (trace where the Framework picks its default serializer when nothing is registered, file:line), on all four paths: source encode, target decode, target reply encode, source completion decode. No codec registration, no manual encode/decode at call sites.
2. Check L5~L7 (2b6da5af64): it must go through the public Framework route with the same default JSON serializer (no protobuf registration). Fix it in that commit if not.
Fixup into the L4 (and if needed L5~L7) commit; replay non-interactively (GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash is fine). Release build at L4 and at the tip; WireTests and all BoundaryTests at the tip; a dry server start/stop with no load at L4 and L5~L7.

## Output
/root/jobs/fx/cx/v21/s033/lad-dotnet-l4j/report.md (Korean, short): default serializer production file:line, bench file:line per path, old→new commit map, build/test evidence. Time cap 2 h.
