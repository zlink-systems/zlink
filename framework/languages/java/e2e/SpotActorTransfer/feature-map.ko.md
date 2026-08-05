# Java SpotActorTransfer E2E feature map

## Deferred Actor Join component 증거

- `ZLinkDeferredActorJoinScopeTest`는 User·Entry Spot handler 한 번에서 서로 다른
  member Actor의 intent를 여러 개 등록하고, handler 정상 terminal 뒤 등록 순서대로
  barrier가 활성화되는지 확인한다.
- `ZLinkAsyncSerialQueueTest.lifecycleBarrierRunsAfterActiveTurnAndBeforeQueuedApplicationTurns`
  는 각 Actor barrier가 현재 실행 중인 turn 다음, 이미 대기 중인 application turn
  앞에서 실행되는지 확인한다.
- `ZLinkDeferredJoinAcceptedRecoveryTest`는 cross-node Accepted completion의
  `OperationId`, raw reply, manifest cursor와 기존 `ObjectGeneration`을 Relocation
  Store root에 보존하고, callback 실패 뒤 같은 operation을 다시 Actor mailbox에
  제출하며 성공한 operation을 중복 제출하지 않는지 확인한다.
- `ZLinkActorSpotRoutePacketsTest`는 Relocation Store reference, checksum과 cursor가
  routed relocation commit wire를 왕복하는지 확인한다.
- 이 test는 component 회귀다. 실제 process 종료 뒤 target recovery와 E2E marker를 검증하지 않으므로
  Config 10의 `ST-H1~H5` 완료 증거로 사용하지 않는다.

기준 문서: `framework/doc/framework/common/e2e/config-10-spot-actor-relocation.ko.md`

이 문서는 Config 10의 계약 시나리오와 Java E2E의 현재 검증 범위를 연결한다. `run_e2e.sh all`이
실행하지 않는 시나리오는 component test가 있더라도 `미구현`으로 기록한다.

| 시나리오 | 상태 | 현재 검증 범위 |
|----------|------|----------------|
| ST-A1 | focused 구현 | Java·Kotlin focused process가 ST-A1 전용 placement fixture에서 `actor-a`를 같은 node target으로 사용하고, public actor API로 target admission, source leave, location commit, target joined와 성공 응답 marker를 확인한다. 현재 실행은 `scenario ST-A1 passed`와 aggregate passed marker까지 통과했다. 전체 transfer ordering과 후속 scenario는 별도 검증이 필요하다. |
| ST-A2 | 구현 | local join 거절 뒤 source membership을 유지하고 leave·joined 부수 효과가 없는지 확인한다. |
| ST-A3 | 구현 | joined callback 대기 중 packet 완료를 차단하고, callback 이후 target에서 처리되는지 확인한다. |
| ST-B1 | 구현 | remote transfer와 state 복원, 역할별 callback 순서, transfer id로 연결된 typed flow marker와 commit ack를 확인한다. |
| ST-B2 | 구현 | commit ack 뒤 source process를 종료해도 target actor와 성공 결과가 유지되는지 확인한다. |
| ST-B3 | 구현 | transfer adapter가 없는 actor가 기본 빈 state로 transfer되는지 확인한다. |
| ST-B4 | 구현 | 명시적인 빈 transfer state와 target domain state를 확인하고, 역할별 callback과 typed flow marker를 같은 transfer id로 연결한다. |
| ST-C1 | 구현 | target admission 뒤 source process를 종료하고, commit되지 않은 target actor와 callback 부수 효과가 없는지 확인한다. |
| ST-C2 | 구현 | target commit 뒤 source process를 종료해도 target actor가 유지되는지 확인한다. |
| ST-C3 | 구현 | transfer-out, leave, transfer-in, joined callback 실패를 각각 주입하고 실패 evidence를 확인한다. |
| ST-D1 | 구현 | joined callback 대기 중에는 target location이 공개되지 않고 callback 완료 뒤 target owner로 바뀌는지 확인한다. |
| ST-D2 | 구현 | stale source release가 새 generation의 target location을 제거하지 못하는지 확인한다. |
| ST-E1 | 구현 | remote transfer 전후 bound session push가 source에서 target으로 이어지는지 확인한다. |
| ST-E1A | runtime contract 구현 | `ZLinkSessionActorBindingContractTest`가 same-generation route replacement와 stale binding 격리, 새 generation의 explicit bind를 검증한다. Process 간 relocation E2E는 아직 필요하다. |
| ST-E2 | 구현 | 실패한 transfer 뒤 bound session push가 기존 source actor로 전달되는지 확인한다. |
| ST-F1 | 구현 | moving 중 packet을 target backlog에 적재하고 arrival index 순서대로 handler에서 다시 처리하는지 확인한다. target `backlog_enqueued` evidence 뒤 replay되는 경로까지 검증한다. |
| ST-F2 | 구현 | handoff backlog를 location publish 전에 target에 적재하고, target direct packet보다 먼저 처리하는지 확인한다. |
| ST-F3 | 구현 | bound session packet이 transfer 전후 순서를 유지하며 target에서 다시 처리되는지 확인한다. |
| ST-F3A | 미구현 | Session owner pause와 owner lease fence를 실제 process에서 검증하는 시나리오가 없다. |
| ST-F4 | 전환 대상 | 이전 시나리오는 caller가 old `ActorRef` route를 직접 지정했다. Global Actor ID로 제출한 operation의 transport delivery를 지연하는 fixture가 필요하다. |
| ST-F5 | 전환 대상 | 연속 relocation 뒤 route 제거 검증도 old `ActorRef` 직접 주입에 의존한다. Public route를 노출하지 않는 delivery-delay fixture로 바꿔야 한다. |
| ST-F6 | 구현 | handoff 중 request reply correlation, 원래 timeout, late reply의 단일 처리를 확인한다. |
| ST-G1 | 미구현 | SpotWide·PerActor의 yielded continuation과 모든 실행 lane을 포함한 relocation barrier E2E가 없다. |
| ST-G2 | 미구현 | 큰 participant inventory와 typed capacity aggregate all-or-none E2E가 없다. |
| ST-G3 | 미구현 | PerActor Spot authority 선전환과 Actor별 source·target route 분할 E2E가 없다. |
| ST-G4 | 미구현 | relocation 중 stale `ToActor` Message Follow와 target queue 순서를 검증하는 E2E가 없다. |
| ST-G5 | 미구현 | Entry·PerActor Actor relocation interruption 목표와 초과 시 계속 진행을 검증하는 E2E가 없다. |
| ST-G6 | 미구현 | `ApplicationSignaled` readiness와 completion callback의 source·target owner를 검증하는 E2E가 없다. |
| ST-H1 | 미구현 | Deferred Join 등록, immutable request와 Actor queue barrier를 실제 process에서 검증하지 않는다. |
| ST-H2 | 미구현 | Join completion outcome, operation ID와 crash recovery E2E가 없다. |
| ST-H3 | 미구현 | Context identity와 relocation 이후 source fence E2E가 없다. |
| ST-H4 | 미구현 | 허용 execution context, 중복 등록과 relocation error parity E2E가 없다. |
| ST-H4A | 미구현 | Deferred Join 등록량·payload·timeout 경계와 Relocate·Shutdown race E2E가 없다. |
| ST-H4B | 미구현 | Join 뒤 Yield, awaited cycle과 reply terminal E2E가 없다. |
| ST-H5 | 미구현 | MessageContext와 Actor handler signature parity를 실제 transport로 검증하는 E2E가 없다. |
| ST-I1 | 미구현 | 실제 encoded Actor·Spot payload profile과 경계·초과 크기 E2E가 없다. |
| ST-I2 | 미구현 | 다량 `RecreateOnRelocation`·`PreserveStateWith` Actor relocation의 처리 시간과 Actor·control service 연속성 E2E가 없다. |
| ST-I3 | 미구현 | 다량 Instance Spot·SpotWide relocation의 처리 시간과 Spot·Actor·control service 연속성 E2E가 없다. |
| ST-I4 | 미구현 | Actor·Spot × one-way·request × commit 전·후 Message Follow matrix가 없다. |
| ST-I5 | 미구현 | Message Follow 기간 종료, duplicate, deadline, generation, loop와 bound E2E가 없다. |
| ST-I6 | 미구현 | Actor·Spot multi-hop relocation과 Message Follow route 정리 E2E가 없다. |

## 남은 검증 갭

- 위 `ST-A1`의 callback과 location commit 순서는 E2E-JV-31에서 추적한다. 성공 응답 marker가 있다는
  사실만으로 10.0.0 순서 계약을 완료로 판정하지 않는다.
- 현재 `run_e2e.sh all`은 `ST-F3A`, `ST-G1~G6`, `ST-H1~H5`, `ST-I1~I6`을 실행하지 않는다. 기존 실행 결과는
  현행 Config 10 전체 완료 증거가 아니다.
