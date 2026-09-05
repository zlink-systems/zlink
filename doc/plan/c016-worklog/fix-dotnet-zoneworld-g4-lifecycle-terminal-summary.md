# .NET ZoneWorld G4 lifecycle terminal 진단

2026-09-05 기준 **진단 완료, runtime 구현과 최종 gate 미실행**이다. 감독에게 B(기존 결함)
진단 승인을 요청했으며, 승인 응답은 아직 없다. 기존 durable 테스트는 새 package로 16/0이다.

## 소유 계층과 계약

- 소유 계층: Framework durable operation은 replay·deadline·terminal을, 기존 mesh/Location runtime은 logical lifecycle을 소유한다. Core/binding은 physical pair 종료와 typed completion을 소유한다.
- Spec 조항: `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:324`의 owner 종료 행과 `04-actor-model.ko.md:662`의 sender replay·remaining deadline·exhaustion 규칙.
- 교차언어 대조: Node ActorJoin은 durable replay를 사용하지 않는다. Java durable sender에는 lifecycle-terminal 판정이 없고, Java sample은 `DEADLINE_EXCEEDED`도 `Unavailable`로 변환한다. 동일한 runtime 규칙의 parity는 확인되지 않았다.
- 변경 분류: **B 제안 — 기존 결함.** 실제 runtime 변경은 아직 없다. 다른 언어의 G4 통과를 즉시 lifecycle-terminal 구현의 근거로 사용할 수 없다.

## 원인과 증거

기존 증거 root는 `scratchpad/fix-dotnet-shoppingmall-relocation-root-and-final-gate/`다.
`g4-terminal/evidence/ZoneWorld/logs/zone-node-1.log`에서 actor `g4-crash-681980`,
flow `01a07039-02f3-798e-bfb0-00eb5675c161`을 확인했다.

| 위치 | 관측 |
|---|---|
| 로그 `:577` | admitted operation `2f5daad9593649a50000000000000020`이 98.0194 ms에 Core `NotConnected/109`를 받는다. |
| 로그 `:580` 이후 | 같은 operation의 native resubmit이 submit `NotConnected/2`로 거부된다. |
| 로그 `:630` | target mesh peer가 `disconnect=False`로 제거된다. |
| 로그 `:1401` | auto-connect target이 제거된다. |
| 로그 `:1594`, `:1607` | deferred join이 `DeadlineExceeded/TaskCanceledException`으로 실패하고 probe response를 전송한다. |

현재 source 기준 원인은 다음과 같다. 경로의 공통 prefix는
`framework/languages/dotnet/src/Zlink.Framework/Runtime/`이다.

| 파일:행 | 원인 |
|---|---|
| `Messaging/ZLinkDurableRequest.cs:31` | typed request/submit failure만으로 replay하며 fixed target lifecycle 종료와 구분하지 않는다. |
| `Service/ZLinkManagedMeshNode.cs:9203` | peer가 없으면 `RequireDirectPeer`가 replay 가능한 submit `NotConnected`를 낸다. |
| `Service/ZLinkManagedMeshNode.cs:9204` | 서로 다른 target lifecycle generation도 같은 replay 가능한 submit failure로 처리한다. |
| `Actors/ZLinkDeferredActorJoin.cs:228` | deferred join 자체 deadline CTS를 만들고 `:276`, `:283`에서 remote join에 전달한다. admission wait까지 취소된다. |
| `Host/ZLinkActorRemoteJoiner.cs:162`, `:187` | remote transaction의 linked deadline CTS가 추가로 존재한다. |
| `Host/ZLinkActorRemoteJoiner.cs:1046` | native completion을 기다리는 상위 Task가 전달받은 cancellation으로 종료될 수 있다. |

Remote join은 admission에 별도 `admissionCancellationToken`을 전달하지만, 이 토큰도
deferred join의 deadline 토큰이다. Admission 대기 중 직접 경쟁하는 것은 sender와 deferred
join이다. Remote wrapper의 CTS도 동작하지만 admission에는 전달되지 않고 이후 transaction
단계에 적용된다. 따라서 remote wrapper 하나만 제거해도 admission 경쟁은 남는다.
Canonical managed operation은 `ZLinkManagedMeshNode.cs:2289`에서 `ownsDeadline: true`를
사용하므로 `:4516`의 별도 operation expiry timer는 설치하지 않는다.

## 교차언어 대조 상세

### Node

`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4068`
의 durable loop 대상에는 ActorJoin이 없다. ActorJoin은 `:4250`의 단일
`raw.requestService`를 사용하고 실패를 completion으로 전달한다.
`runtime/actors/actor-local-native-join.ts:643`은 timeout completion에서 기존 `node.peers()`의
admitted state·RID·lifecycle generation을 확인하고, 일치하는 peer가 없으면
`RouteNotConnected`로 변환한다. 이는 lifecycle 종료를 관찰하여 durable replay를 즉시
중단하는 구현과 다르다.

Node sample runner `framework/languages/node/samples/ZoneWorld/Runner/sample-runner.mjs:216`
이후는 crash 뒤 B4/C2/C3 및 previous-owner probe를 확인하고 replacement를 시작한다.
`Client/special.ts:117`의 previous-owner probe는 장애 상태를 관찰한 **뒤** 제출된다.
`:322`의 G4 client는 replacement의 새 Actor 생성·route를 검증한다. .NET처럼 target
join callback이 실행 중인 경계에서 crash하는 검사와 동일하지 않다.

### Java

`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/` 기준으로
`runtime/actors/ZLinkActorSpotJoinCall.java:701`은 canonical transport completion을 기다린다.
`runtime/binding/ZLinkJavaRawMeshNode.java:1409`는 durable sender를 호출한다.
`ZLinkJavaDurableRequest.java:87` 이후는 admitted `NOT_CONNECTED`/`TIMED_OUT`과 submit
실패를 replay하고 `:125` 이후 exhaustion에서 admission 이력에 따라 오류를 고른다.
이 sender에는 target lifecycle 종료 판정이 없다.

Java G4는 실제 in-flight crash를 검사하지만, sample의
`framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/zone/actors/PlayerActor.java:222`
가 `UNAVAILABLE`, `DEADLINE_EXCEEDED`, `SHUTTING_DOWN`을 모두 문자열 `Unavailable`로
변환한다. 따라서 Java G4 통과는 runtime의 `UNAVAILABLE` 즉시 종결을 증명하지 않는다.
이번 작업에서 Java나 sample을 수정하지 않았다.

## 수정 대안과 검토 경계

- Peer 부재 또는 `!Admitted`를 즉시 terminal로 사용하는 대안은 채택하지 않는다.
  `ZLinkManagedMeshNode.cs:8593`의 disconnect drain은 transient inbound disconnect에서도
  `RemovePeer(..., disconnect: false)`를 호출한다. 물리 연결 종료를 process 종료로
  간주하면 요청된 transient replay 보장을 깨뜨린다.
- 제안은 기존 mesh/Location의 generation 교체·lifecycle 제거 결정을 durable operation에
  연결하고, admission 중에는 sender가 deadline과 terminal을 소유하도록 하는 것이다.
  새 monitor 상태·timer·poller·retired-generation mapping을 만들지 않는다. Predecessor,
  authority I/O, admission 이후 relocation의 기존 deadline 보장은 별도로 보존해야 한다.

기존 lifecycle 입력은 `ZLinkLocationAutoConnectHost.cs:681`의 expectation 등록과 `:694`의
제거, `ZLinkManagedMeshNode.cs:409`의 expectation generation,
`:438`의 connection intent 제거다. 단, auto-connect의 row 부재만으로도 expectation은
제거될 수 있으며 live admitted peer는 유지한다(`ZLinkLocationAutoConnectHost.cs:696`).
따라서 expectation 부재 단독 판정도 lifecycle 종료의 충분한 증거가 아니다.

수정 전/후 규칙 수: **실제 변경 없음**. Admission terminal에 직접 경쟁하는 deadline
소유자는 현재 2곳(sender, deferred join)이며 목표는 1곳이다. Remote transaction의
deadline CTS까지 포함한 동시 deadline 예산 관리 지점은 3곳이다.
Lifecycle-terminal 규칙은 목표 1개이며 transport 부재와 logical lifecycle 종료를
구분하는 기존 소유 경계를 확정한 뒤 구현한다.

## 검증 결과

`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/rebuild7.done`
존재 확인 뒤에만 .NET 명령을 실행했다. Nupkg Linux x64 libzlink와
`core/build-dev/lib/libzlink.so`의 SHA-256은 모두
`1c7887c36dd2f1fe133fe3a39ccbfeb9ea42d4b051b302e3ecacf160e31b116d`이며 이전 `c5f62fb1…`과 다르다.
Nupkg SHA-256은 `a46acff62126d5ed4cad8f4bfc0fe14ea246587d4499d34cb415bd0b1d513817`이다.

지정된 TMPDIR, ZLINK_LIBRARY_PATH, compilation/node reuse 환경과 package hash별
`NUGET_PACKAGES`를 사용했다. 명령은 `framework/languages/dotnet`에서
`flock -w7200 /tmp/zlink-dotnet-gate.lock dotnet test tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj --filter 'FullyQualifiedName~DurableRequestTests|FullyQualifiedName~DurableSenderPreservesExhaustionCauseAndOriginalOperation' --logger 'console;verbosity=minimal'`이다.

| 검증 | 결과 | 증거 |
|---|---|---|
| 기존 durable focused baseline | 16 passed / 0 failed / 0 skipped, net8.0, test duration 14 s | `/tmp/zlink-g4-durable-baseline.log` |
| 신규 lifecycle 회귀 테스트 | 미작성·미실행 | runtime 진단 승인 대기 |
| ZoneWorld 1회·2회 | 미실행 | 수정 후 gate |
| `bash samples/run_samples.sh` aggregate | 미실행 | 수정 후 gate |
| unit half `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests` | 미실행 | 수정 후 gate |

Baseline build에서 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning이 관찰됐으며 수정하지 않았다.
기존 `DurableSenderRuntimeTests` assertion도 그대로다.

## BLOCKERS

- Root `AGENTS.md:71`의 “감독이 A 또는 B로 승인한 뒤에만 2단계 구현을 시작한다”에 따른
  B 진단 승인 응답을 기다린다. Runtime code는 변경하지 않았다.
- “Node/Java와 동일한 lifecycle-terminal 구현”이라는 전제가 source 대조로 확인되지 않았다.
  Node의 다른 submit 경로와 Java sample의 오류 통합을 복제하면 이번 요구를 충족하지 못한다.
- 기존 peer/expectation/intent의 어느 전이가 process lifecycle 종료의 충분한 증거인지
  구현 승인과 함께 확정해야 한다. 단순 peer 부재는 transient disconnect도 포함한다.
- 현재 G4 결함은 해결되지 않았다. 최종 sample·unit gate와 역할별 `Stopped/None`은 검증하지 않았다.

변경 파일은 이 진단 보고서뿐이며 commit은 없다. 기존 binding provenance 변경,
staged prompt와 untracked `opah/`, `zlink-work/`는 보존했다.
