Model gpt-6-luna, effort high: reproduce one concurrency scenario through public APIs in two languages with existing test infrastructure; no product change. Earlier job briefs are history, not instructions.

# Task: #1639 same-owner defect — public-API repro in .NET and Java (C++ is done)

Folder JOB=/root/jobs/fx/cx/v21/s033/sameowner3. Worktree /root/worktree/zlink-v21-s033/sameowner3 (detached at origin/main). Read first: /root/jobs/fx/cx/v21/s033/sameowner/report.md and repro.diff (the C++ test: ClientServer channel without a gate; handler for request id 1 blocks on a latch the test controls; send request 2 to the SAME channel and wait 250 ms for its handler to start; then release; control: request 2 to a DIFFERENT channel; 20 iterations each; C++ result 0/20 same channel vs 20/20 different channel).

## Do
1. .NET: the same scenario through the public Framework API in the existing test project that hosts ClientServer channels (find a test that adds a client-server channel and a handler group; copy its setup). Record same-channel and different-channel start counts out of 20. Also a gated control: an Actor (or Spot) handler blocked on request 1, request 2 to the same Actor must NOT start before release (20×). If an Actor host is costly to set up, find the existing test fixture that does it.
2. Java: the same three cases in zlink-framework-core tests (find an existing ClientServer channel test and an Actor test fixture).
3. For each language: the file:line that decides whether the second record of the same owner can be taken while the first is active (candidates: .NET ZLinkMeshOwnedMailbox.cs:104 + ZLinkManagedMeshNode.cs:2833 + ZLinkMeshNodeRouteDispatcher.cs:247; Java ZLinkServiceMailbox.java:56 + ZLinkHandlerDispatch.java:63), and the mechanism in one line.
4. One line for Node: there is one event loop, so "another worker runs the next record before the previous handler suspends/completes" — state what Node does when handler A awaits (does B start while A is suspended?) by source reading with file:line, and whether that satisfies S02 §2 (framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:97-105).
5. Do not change product code. Keep the test sources as JOB/dotnet-repro.diff and JOB/java-repro.diff; outputs as JOB/*.log.
6. Report: 결과(언어별 표: 같은 channel / 다른 channel / gated 대조, 각 20회), 판정 위치(file:line), 분류, 검증(명령·로그), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Core: release package P=/root/.cache/zlink/core-release/1.24.0/linux-x64 with ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=$P (Java Gradle: ./gradlew -q --no-daemon :zlink-framework-core:test --tests '<class>'; .NET: dotnet test with --filter).
- Every claim file:line or log path. Report in Korean, short. Remove build outputs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/sameowner3/report.md
