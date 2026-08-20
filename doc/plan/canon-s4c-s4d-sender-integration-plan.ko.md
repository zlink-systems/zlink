# canon-S4c cpp/dotnet 발신 + S4d relocation-state 통합 계획

작성 2026-08-21 (codex terra high, read-only). Claude 검토.
**해법 = language-internal CanonicalActorJoinAttempt seam(wire 무변경)**. node/java 발신 완료(9126956c4b·c110fd375c), cpp/dotnet은 이 seam 선행 필요.
**cpp 회귀 근본**: canonical branch가 seal_remote_application_actor_join() 건너뛰고 deliver 즉시호출 + admission identity 불일치.
**dotnet 근본**: canonical 28 수신은 reserved actor만 생성, 실제 relocation target은 JSON HandoffId admission/prewarm import 필수 → seam이 canonical accepted context를 저장·command 40에 결합해야.

---


### 1. Node·Java가 결합을 피한 방식

| 언어 | canonical 28이 하는 일 | local에 보존하는 상태 | 실제 commit 소유자 |
|---|---|---|---|
| Node | Store/fence 기반 승인과 application reply | `relocationId`, pending operation→transferId, source relocation profile | `ZLinkHostRelocationRuntime` |
| Java | Store/fence 기반 승인과 application reply | `transferId(UUID)`, public operation ID, target fence, active-turn seal, raw reply, chunk limit | `ZLinkActorJoinRelocationPort` |
| C++ 현재 wire 분기 | 승인 후 즉시 public completion | completion ID 일부만 | 없음 — JSON의 relocation chain을 건너뜀 |
| .NET 현재 JSON | 승인 reply 자체가 target reservation lease | token·payload reservation·target fence·handoffId | `ZLinkActorRemoteJoiner` |

Node는 canonical 전송 전 새 `relocationId`를 만들고, pending operation의 transfer ID를 local map에 보존합니다. canonical 전송이 선택돼도 local map에는 그 ID가 남으며, 뒤이어 host relocation runtime이 같은 ID로 relocation을 실행합니다. [actor-local-native-join.ts](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-local-native-join.ts:350), [service-stateful-runtime.ts](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:4246), [service-relocation-host-runtime.ts](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts:393)

중요하게도 Node target의 canonical admission은 private transfer request를 만들지 않습니다. canonical control은 Store resolver만 통과하고, later relocation stage가 별도 `relocationId`로 target state를 조립·commit합니다. 즉 canonical reply가 reservation token을 돌려주지 않아도 됩니다. [index.ts](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/index.ts:1835), [index.ts](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/index.ts:1850)

Java도 동일합니다. source는 canonical request 전에 UUID transfer ID를 만들고, accepted reply 뒤 `Goal`에 transfer ID, operation ID, source actor/type, target fences, active-turn seal, application reply, chunk cap을 담아 relocation port로 넘깁니다. [ZLinkActorSpotJoinCall.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotJoinCall.java:617), [ZLinkActorSpotJoinCall.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotJoinCall.java:817)

Java target도 canonical admission을 `"canonical:<correlation>"`라는 local placeholder로 표현할 뿐, 명시적으로 private pending-transfer record를 만들지 않습니다. 따라서 target reservation의 identity는 28 reply가 아니라 뒤따르는 canonical relocation state/prepare에서 확정됩니다. [ZLinkSpotRuntime.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotRuntime.java:3604), [ZLinkActorSpotAdmission.java](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkActorSpotAdmission.java:407)

### 2. .NET: 현재 admission reply의 정확한 요구와 canonical tail의 차이

.NET의 `handoffId`는 source에서 먼저 생성됩니다. canonical 전송으로 바뀌어도 이것은 그대로 language-internal relocation identity여야 합니다. [ZLinkActorRemoteJoiner.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs:146)

현재 private admission reply에서 검증·사용하는 값은 다음과 같습니다.

| 값 | 현재 사용처 | canonical 28 tail |
|---|---|---|
| reservation token | 실패/취소 시 exact target reservation abort | 없음 |
| reserved payload bytes | source prediction과 일치 검증, target reservation bound | 없음 |
| target node RID / node generation | target descriptor 재검증, immutable relocation root destination | spot ref에는 node RID 없음 |
| target Spot generation | root destination, prepare/finalize route fence | 있음 (`spot.generation`) |
| target actor authority generation | source→target CAS 기대값, recovery record | 없음 |
| target Spot authority generation / owner lease | target router-route fence, abort route | 없음 |
| receive chunk limit | direct relocation chunk-size clamp | 있음 |
| app reply | 최종 Join reply 및 recovery record | tail 밖 application payload로 가능 |
| handoff/transfer ID | source capture, recovery, seal/abort, relocation record key | 없음, source-local이어야 함 |
| completion operation ID | recovery와 eventual public completion identity | 없음, source-local이어야 함 |
| bound-session accepted high-water | actor state의 bound-session state를 seal/route할 때 사용 | admission reply가 아닌 actor state에서 옴 |

현재 .NET은 accept 직후 token, predicted payload equality, target node/spot/authority generation을 모두 검사하고 `ZLinkActorRelocationReservation`을 만듭니다. [ZLinkActorRemoteJoiner.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs:524), [ZLinkRemoteActorJoinPackets.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkRemoteActorJoinPackets.cs:694)

그 reservation은 두 역할을 섞고 있습니다.

- 취소 전용 lease: token으로 `AdmissionAbort`를 보낸다.
- relocation continuation context: destination/recovery/root/target fence/chunk limit을 구성한다.

첫 역할은 canonical 28로 직접 대체할 수 없습니다. 둘째 역할은 canonical tail의 `spot`, `membershipEpoch`, `receiveChunkLimitBytes`와 source가 이미 가진 fresh target authority snapshot으로 재구성할 수 있습니다. `membershipEpoch`은 commit 완료 증거가 아니라 승인 시 제안된 membership 값으로 취급해야 합니다.

canonical tail은 accepted case에 `spot`, `membershipEpoch`, `receiveChunkLimitBytes`만 둡니다. reservation token, payload budget, target node/owner generations, transfer ID, completion identity는 의도적으로 없습니다. [service-wire-v1.schema.json](/home/hep7/project/zlink/framework/runtime/protocol/service-wire-v1.schema.json:2239)

### 3. C++가 canonical send에서 깨지는 정확한 지점

기존 JSON 경로는 다음의 연속 transaction입니다.

1. source가 `transfer_id`, completion high/low ID, deadline, source authority/fence를 설정한다.
2. private admission packet이 그 transfer ID와 completion ID를 target으로 보낸다.
3. target `actor_transfer_coordinator`가 transfer ID 아래에 source/target spot, deadline, completion IDs, app reply를 parked admission으로 저장한다.
4. source가 seal → `transfer_actor_out` → prepare → finalize/cutover → Core commit → target route publication → completion delivery를 수행한다.

근거는 [mesh_node_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2407), [spot_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6003), [actor_transfer_coordinator.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.hpp:34), [mesh_node_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2755)입니다.

canonical wire branch는 completion IDs를 만들고 canonical request를 보낸 뒤, accepted tail을 받으면 receive chunk limit을 기록하고 곧바로 `deliver_remote_actor_join()`을 호출합니다. `seal_remote_application_actor_join()`와 이후 prepare/finalize chain으로 들어가지 않습니다. [mesh_node_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2583), [mesh_node_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2639)

따라서 Bingo/TicTacToe 회귀의 직접 원인은 “deep completion delivery 실패”가 아니라 다음 세 가지 state 분리입니다.

- source `s->transfer_id`, source spot generation, source authority snapshot, deadline, seal state가 JSON 경로처럼 초기화·소비되지 않는다.
- target은 canonical correlation으로 파생한 `wire-actor-join:<source rid>:<source generation>:<correlation>` ID 아래 admission을 실제로 park한다. source는 그 identity를 알거나 후속 prepare에 사용하지 않는다. [mesh_node_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:517)
- target parked admission은 completion identity exact match를 요구한다. canonical receiver가 저장하는 `{source node generation, correlation}`와 source가 JSON 방식으로 만든 `{mesh hash, counter}`는 다르다. 그러므로 단순히 canonical send 뒤 JSON prepare를 이어도 target prepare identity가 맞지 않는다. [spot_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6089)

또한 현재 HEAD에서는 canonical receiver의 cold target stable-type 조회는 authority row fallback으로 보완돼 있습니다. 과거 revert 설명의 “stable type을 못 찾는다”는 부분은 현 상태의 주 원인이 아닙니다. 실제 남은 핵심은 sender/target reservation identity와 completion identity의 통합입니다. [mesh_node_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:457)

### 4. 통합 설계: `CanonicalActorJoinAttempt` language-internal seam

공통 wire를 늘리지 않고, 각 언어에 내부 attempt record를 둡니다.

```text
canonical request
  correlation + exact actor/source fence + exact target fence
               │
               ▼
CanonicalActorJoinAttempt (language-internal)
  - wireAttemptKey: source actor fence + correlation
  - relocationId / handoffId
  - source public-completion identity
  - target admission/reservation handle or derived key
  - source/target authority snapshots
  - source seal / rollback state
  - target chunk cap and application reply
               │
               ▼
existing relocation prepare → state transfer → target-only CAS
→ message-follow / route handoff → source cleanup → public completion
```

필수 불변식은 다음입니다.

- canonical accepted는 **admission accepted**일 뿐 public Join accepted나 source completion terminal이 아니다.
- `RelocationId`/`handoffId`/reservation handle/completion identity는 28 body나 reply에 넣지 않는다.
- canonical correlation은 wire 재전송 idempotency key이고, public completion ID와 별개다. 둘을 동일시하지 않는다.
- target의 parked admission은 exact `wireAttemptKey` 또는 local reservation handle로 later prepare와 결합한다.
- commit 이전 실패는 target local attempt abort + source seal rollback + bound-session seal abort를 한다.
- target-only CAS 뒤 실패는 source replay로 되돌리지 않고 현 authority를 reconciliation한다.
- `receiveChunkLimitBytes`만 canonical tail에서 relocation state chunking에 반영한다.

Node/Java 패턴은 .NET에 직접 적용 가능합니다. 즉 source-side `CanonicalActorJoinAttempt`가 private JSON reply의 reservation context를 대체해야 합니다. 다만 C++에는 target이 이미 canonical admission 시점에 pending transfer를 만들기 때문에 Node/Java보다 한 단계 더 필요합니다. C++는 target pending admission을 없애거나, canonical attempt key로 later prepare가 그 pending admission을 찾아가도록 bridge해야 합니다. 후자가 기존 actor-transfer coordinator를 보존하므로 위험이 낮습니다.

### 5. 권장 슬라이스

1. S4d seam 먼저: wire 변경 없이 C++/.NET 각각에 internal attempt/context type과 lifecycle test를 만든다.
   - accepted tail 후 public completion이 발생하지 않음
   - commit 전 실패는 target reservation/attempt와 source seal을 모두 정리
   - commit 뒤 실패는 source rollback 금지, authority reconciliation
   - canonical correlation과 public completion ID가 다름

2. C++ target bridge:
   - `wireAttemptKey`를 canonical receiver와 later prepare/finalize가 공통으로 사용하도록 만든다.
   - target coordinator의 parked admission에는 wire identity와 local public completion identity를 분리해 저장한다.
   - canonical accepted 후에는 `seal_remote_application_actor_join()`으로 반드시 진입시키고, 현 즉시 `deliver_remote_actor_join()`은 제거한다.
   - focused transfer/coordinator tests 뒤 Bingo·TicTacToe role-process regression을 별도 증거로 실행한다.

3. .NET admission/relocation 분리:
   - `SubmitRoutedJoinActorTransactionAsync`를 `AcquireAdmissionContext`와 `ContinueRelocation`으로 분해한다.
   - JSON context는 현 token reservation adapter, canonical context는 canonical tail + observed target authority snapshot + local handoff attempt adapter가 되게 한다.
   - abort는 context polymorphism으로 보낸다. canonical context가 private token abort를 흉내 내면 안 되며, target-side canonical attempt abort/recovery를 소유해야 한다.
   - bound-session high-water는 기존 `actorState` 경로를 유지한다.

4. .NET canonical sender:
   - capability/observed-authority gate 아래 28을 보내고, accepted tail을 `CanonicalAdmissionContext`에만 기록한다.
   - 이후 기존 direct relocation prepare/data/cutover/CAS/replay/cleanup을 동일하게 통과시킨다.
   - JSON fallback은 peer capability 또는 incomplete authority evidence에서 그대로 유지한다.

5. 최종 검증:
   - C++/.NET focused seam tests
   - package/build lane
   - C++ Bingo·TicTacToe 및 .NET relocation sample/process lane
   - reject, pre-commit failure, post-CAS ambiguity, bound-session, replayed canonical correlation을 분리해 검증

이번 작업은 읽기 전용 조사만 했으며 파일 변경·커밋·문서 수정은 없습니다.




---

## attempt-binding 검증 결과 (2026-08-21, advisor 처방 2검증 — 둘 다 불성립)

**결론: binding-at-first-40 규칙 미완성.** 3언어 모두 (1) 첫 40이 canonical 28 admission에 full triple pin·이후 다른 triple 40을 typed stale terminal 거부 = 없음, (2) later-attempt-wins 재park의 prior-40-binding 무효화 + superseded source terminal 구분 = 없음. **필요 = 신규 attempt-lifecycle 레이어**(admission attempt registry per (actorId,objectGeneration) + 첫 40 원자 pin + later 28 supersede + 명시 53 stale/superseded terminal + source rollback 의미 보존) — 3언어 부재. 이것은 스펙 단일 판정이 아니라 **집중 프로토콜-인프라 설계**(loop 슬라이스 불가).

