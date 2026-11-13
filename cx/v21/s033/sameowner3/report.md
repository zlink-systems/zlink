# #1639 same-owner 공개 API 재현

## 결과

| 언어 | 같은 channel/owner | 다른 channel/owner | gated Actor 대조 |
|---|---:|---:|---:|
| .NET | 0/20 | 20/20 | 0/20 |
| Java | 0/20 | 20/20 | 0/20 |
| C++ 선행 재현 | 0/20 | 20/20 | 미실행 |

.NET과 Java 모두 같은 channel의 두 번째 handler는 첫 handler가 latch에서 막힌 동안 시작하지 않았고, 다른 channel은 시작했다. Gated Actor에서는 두 언어 모두 두 번째 요청이 release 전에 시작하지 않았다. 이로써 .NET/Java의 gate-less channel 동작은 C++ 공개 API 재현과 일치하며, Actor의 gate 대조도 기대대로 직렬화됐다. [dotnet-repro.log](/root/jobs/fx/cx/v21/s033/sameowner3/dotnet-repro.log:1), [java-repro.log](/root/jobs/fx/cx/v21/s033/sameowner3/java-repro.log:1), [C++ 선행 report](/root/jobs/fx/cx/v21/s033/sameowner/report.md:5).

## 판정 위치와 기제

- .NET 판정 위치: [ZLinkMeshOwnedMailbox.cs:104](/root/worktree/zlink-v21-s033/sameowner3/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshOwnedMailbox.cs:104), [ZLinkManagedMeshNode.cs:2833](/root/worktree/zlink-v21-s033/sameowner3/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:2833), [ZLinkMeshNodeRouteDispatcher.cs:247](/root/worktree/zlink-v21-s033/sameowner3/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:247); 기제: ready owner를 claim하고 현재 batch의 record 수를 고정하므로 뒤늦게 온 같은 owner record는 claim 완료 후 처리된다.
- Java 판정 위치: [ZLinkServiceMailbox.java:60](/root/worktree/zlink-v21-s033/sameowner3/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceMailbox.java:60), [ZLinkHandlerDispatch.java:67](/root/worktree/zlink-v21-s033/sameowner3/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkHandlerDispatch.java:67); 기제: claimed owner는 다시 claim하지 않고 handler 완료 때 기존 claim을 닫는다.
- 분류: **B — 기존 계약 결함**. .NET/Java/C++에서 gate-less 동일 owner의 다음 record가 active handler 완료 전 시작하지 않는 결과가 재현됐다. S02 §2는 Node/Channel handler에 gate가 없고 다음 pipeline이 앞 handler의 suspend/완료 전에 다른 worker에서 실행될 수 있다고 정한다([spec:97](/root/worktree/zlink-v21-s033/sameowner3/framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:97)). 제품 코드는 변경하지 않았다.
- Node: 단일 event loop지만 [mesh-dispatch-pump.ts:41](/root/worktree/zlink-v21-s033/sameowner3/framework/languages/node/packages/framework/src/runtime/backend/mesh-dispatch-pump.ts:41)는 application worker를 최소 2개 두고, [channel-dispatch-pipeline.ts:392](/root/worktree/zlink-v21-s033/sameowner3/framework/languages/node/packages/framework/src/runtime/channels/channel-dispatch-pipeline.ts:392) handler Promise를 await하므로 A가 suspend된 동안 B가 시작할 수 있다. 이는 event-loop 내 비동기 interleaving이며 S02 §2를 만족한다([spec:103](/root/worktree/zlink-v21-s033/sameowner3/framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:103)).

## 검증 및 산출물

- .NET: `ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64 dotnet test tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj --filter FullyQualifiedName~SameOwnerPublicApiReproTests --no-restore --logger 'console;verbosity=detailed'` — 1/1 통과.
- Java: `ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64 ./gradlew -q --no-daemon :zlink-framework-core:test --tests 'systems.zlink.framework.runtime.channels.ZLinkSameOwnerPublicApiReproTest'` — 1/1 통과.
- 테스트 변경만 [dotnet-repro.diff](/root/jobs/fx/cx/v21/s033/sameowner3/dotnet-repro.diff)와 [java-repro.diff](/root/jobs/fx/cx/v21/s033/sameowner3/java-repro.diff)에 보관했다. 실행 결과는 위 로그 파일에 있다.
- 리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 0.

## 남은 문제

- gate-less channel의 same-owner active handler 중첩 결함은 재현됐다. 제품 수정은 이번 범위에 포함하지 않았다.
- Actor gated 대조는 두 언어에서 모두 20회 직렬 실행을 확인했다.
