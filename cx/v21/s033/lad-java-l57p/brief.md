Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: Java ladder L5~L7 — turn OFF the ZLinkProtobufCodec registration when ZLINK_BENCH_LADDER_L5_7 is set (server and client), as C++ and Node do. IMPLEMENTATION ONLY, NO MEASUREMENT. EXPERIMENT BRANCH ONLY.

Worktree /root/worktree/zlink-v21-s033/lad-java-l57p (NEW, separate), branch exp/4350-java-ladder-l57p, HEAD f3e3933567 (= exp/4350-java-ladder tip: L5~L7). Another job (lad-java-s2) is MEASURING from /root/worktree/zlink-v21-s033/lad-java right now: never touch that worktree, its build outputs or its branch. Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Do NOT take /root/jobs/fx/cx/measure.lock, run no benchmark or load. Never delete raw files.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md, ladder-steps.md (L4 serializer table), lad-java-impl5/report.md + call-shape-L5-7.md, and for reference C++ 62e766f88d (bench_framework_cpp_server.cpp: `if (level != 7) codecs.use(protobuf)`) and Node 7d1afa98a9 (git show only).

## Do
PM decision 11-13 19:00: #4354 = no codec registration; make the four languages' settings the same. In FrameworkStack.java (client, options.codecs().use(ZLinkProtobufCodec.defaultCodec()) around :268) and ZLinkFrameworkBenchServer.java (server, around :444), skip that registration when ZLINK_BENCH_LADDER_L5_7 is set; the typed BenchTypedPayload DTO then goes through the default JSON serializer exactly as now. Make sure nothing else on the L5_7 path needs the protobuf codec (if the server also registers protobuf-typed BenchPayload handlers that need it, register those handlers only when L5_7 is off, and say so). Keep non-ladder/main behaviour unchanged.
One commit "bench(java): no protobuf codec registration in ladder L5_7 (#4354)". bash framework/bench/grpc/java/build.sh in THIS worktree, bench checks/tests, and a dry server start/stop with ZLINK_BENCH_LADDER_L5_7 set and no load (/ready, clean drain).

## Output
/root/jobs/fx/cx/v21/s033/lad-java-l57p/report.md (Korean, short): what changed file:line, commit id, build/test/dry-start evidence, and the exact install dirs of the built client/server so the measurement job can run L5~L7 from this worktree. Time cap 1 h.
