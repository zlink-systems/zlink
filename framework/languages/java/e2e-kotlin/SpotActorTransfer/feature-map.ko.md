# Kotlin SpotActorTransfer E2E feature map

## Deferred Actor Join Kotlin projection 증거

- `KotlinDeferredActorJoinCompletionTest`는 Java runtime이 복구해 제출한
  `Accepted` completion을 suspending Actor callback이 같은 `OperationId`, raw reply와
  `ObjectGeneration`으로 받는지 확인한다.
- Kotlin은 Java의 동기 `defer()`와 recovery runtime을 그대로 사용하며 별도
  coroutine terminal이나 별도 completion type을 추가하지 않는다.
- process 종료를 포함한 실제 cross-node recovery는 Java lane과 같은 Config 10
  scenario가 활성화된 뒤 E2E 완료로 판정한다.

기준 문서는 [Config 10 — Spot·Actor relocation](../../../../doc/framework/common/e2e/config-10-spot-actor-relocation.ko.md)이다.
Kotlin lane은 Java와 shared transfer fixture를 사용하더라도 Kotlin client와 server entry point에서
각 정식 시나리오를 독립적으로 증명해야 한다.

| 시나리오 | 상태 | 검증 대상 |
|---|---|---|
| `ST-A1` | focused 구현 | Kotlin focused process가 ST-A1 전용 placement fixture에서 `actor-a`를 같은 node target으로 사용하고, `getOrCreate(...).request(...).submit()` public path로 transfer와 global Actor ID request를 확인한다. `scenario ST-A1 passed` 및 aggregate passed marker를 반환하지만 후속 transfer scenario와 전체 relocation evidence는 남아 있다. |
| `ST-A2` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: local admission reject의 무효과. |
| `ST-A3` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: joined callback 완료 전 packet dispatch 차단. |
| `ST-B1` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: remote transfer 성공과 state 복원. |
| `ST-B2` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: commit 뒤 source cleanup. |
| `ST-B3` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: adapter 미등록 상태에서도 framework 기본 빈 state transfer 성공. |
| `ST-B4` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: custom empty state transfer. |
| `ST-C1` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: commit 전 source 종료. |
| `ST-C2` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: commit 뒤 source 종료. |
| `ST-C3` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: callback 단계별 failure 분류. |
| `ST-D1` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: location commit 공개 시점. |
| `ST-D2` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: stale generation fencing. |
| `ST-E1` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: transfer 성공 뒤 bound session push. |
| `ST-E1A` | runtime contract 구현 | 공용 JVM runtime test와 Kotlin projection test가 이전 generation binding event 격리와 exact generation logical notification을 검증한다. Process 간 relocation E2E는 아직 필요하다. |
| `ST-E2` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: transfer 실패 때 기존 session binding 유지. |
| `ST-F1` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: moving backlog FIFO. |
| `ST-F2` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: location publish 전 replay 순서. |
| `ST-F3` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: bound session의 cross-move FIFO. |
| `ST-F4` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: 제한된 Message Follow 기간. |
| `ST-F5` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: Message Follow route 교체와 만료 뒤 제거. |
| `ST-F6` | 전환 필요 | MeshNode topology와 공개 API로 검증할 대상: request correlation과 caller timeout 뒤 late reply 격리. |
| `ST-F3A` | 미구현 | Session owner pause와 owner lease fence를 실제 process에서 검증하는 시나리오가 없다. |
| `ST-G1~G6` | 미구현 | relocation barrier, aggregate capacity, `PerActor` route 분리, interruption 목표와 application-signaled 경계를 검증하지 않는다. |
| `ST-H1~H5` | 미구현 | Deferred Join의 queue barrier, durable completion, execution context와 `MessageContext` parity를 검증하지 않는다. |
| `ST-I1` | 미구현 | 실제 encoded Actor·Spot payload profile과 permit 경계를 측정하지 않는다. |
| `ST-I2` | 미구현 | 다량 Actor relocation의 처리 시간과 서비스 연속성을 측정하지 않는다. |
| `ST-I3` | 미구현 | 다량 Instance Spot·`SpotWide` User Spot relocation의 처리 시간과 서비스 연속성을 측정하지 않는다. |
| `ST-I4` | 미구현 | Actor·Spot one-way·request의 authority commit 전후 Message Follow matrix가 없다. |
| `ST-I5` | 미구현 | Message Follow expiry, duplicate, deadline, generation, loop와 bound를 검증하지 않는다. |
| `ST-I6` | 미구현 | Actor·Spot multi-hop Message Follow와 route cleanup을 검증하지 않는다. |

Kotlin Client·Shared·JavaClient·ActorNode는 현재 source로 compile된다. Runner는 Java의
`ST-A1~F6` process 구성을 사용하지만 `ST-A1`에서 위 remote Actor dispatch gap을 재현하므로
그 뒤 시나리오와 `all`을 완료 증거로 사용할 수 없다. `ST-F3A`, G·H·I track은 selector에도
등록되어 있지 않다.

각 시나리오는 실제 Kotlin client와 server entry point에서
`ST-* result=passed` marker를 남겨야 한다. 최종
`spot-actor-transfer e2e result=passed` marker, bindings package 이름·version·경로와
제거 대상 topology의 정적 검사 결과도 함께 출력해야 한다.
