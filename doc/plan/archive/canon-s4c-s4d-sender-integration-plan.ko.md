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

Node는 canonical 전송 전 새 `relocationId`를 만들고, pending operation의 transfer ID를 local map에 보존합니다. canonical 전송이 선택돼도 local map에는 그 ID가 남으며, 뒤이어 host relocation runtime이 같은 ID로 relocation을 실행합니다. [actor-local-native-join.ts](../../../framework/languages/node/packages/framework/src/runtime/actors/actor-local-native-join.ts#L350), [service-stateful-runtime.ts](../../../framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts#L4246), [service-relocation-host-runtime.ts](../../../framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts#L393)

중요하게도 Node target의 canonical admission은 private transfer request를 만들지 않습니다. canonical control은 Store resolver만 통과하고, later relocation stage가 별도 `relocationId`로 target state를 조립·commit합니다. 즉 canonical reply가 reservation token을 돌려주지 않아도 됩니다. [index.ts](../../../framework/languages/node/packages/framework/src/runtime/spots/index.ts#L1835), [index.ts](../../../framework/languages/node/packages/framework/src/runtime/spots/index.ts#L1850)

Java도 동일합니다. source는 canonical request 전에 UUID transfer ID를 만들고, accepted reply 뒤 `Goal`에 transfer ID, operation ID, source actor/type, target fences, active-turn seal, application reply, chunk cap을 담아 relocation port로 넘깁니다. [ZLinkActorSpotJoinCall.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotJoinCall.java#L617), [ZLinkActorSpotJoinCall.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotJoinCall.java#L817)

Java target도 canonical admission을 `"canonical:<correlation>"`라는 local placeholder로 표현할 뿐, 명시적으로 private pending-transfer record를 만들지 않습니다. 따라서 target reservation의 identity는 28 reply가 아니라 뒤따르는 canonical relocation state/prepare에서 확정됩니다. [ZLinkSpotRuntime.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkSpotRuntime.java#L3604), [ZLinkActorSpotAdmission.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkActorSpotAdmission.java#L407)

### 2. .NET: 현재 admission reply의 정확한 요구와 canonical tail의 차이

.NET의 `handoffId`는 source에서 먼저 생성됩니다. canonical 전송으로 바뀌어도 이것은 그대로 language-internal relocation identity여야 합니다. [ZLinkActorRemoteJoiner.cs](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs#L146)

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

현재 .NET은 accept 직후 token, predicted payload equality, target node/spot/authority generation을 모두 검사하고 `ZLinkActorRelocationReservation`을 만듭니다. [ZLinkActorRemoteJoiner.cs](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs#L524), [ZLinkRemoteActorJoinPackets.cs](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkRemoteActorJoinPackets.cs#L694)

그 reservation은 두 역할을 섞고 있습니다.

- 취소 전용 lease: token으로 `AdmissionAbort`를 보낸다.
- relocation continuation context: destination/recovery/root/target fence/chunk limit을 구성한다.

첫 역할은 canonical 28로 직접 대체할 수 없습니다. 둘째 역할은 canonical tail의 `spot`, `membershipEpoch`, `receiveChunkLimitBytes`와 source가 이미 가진 fresh target authority snapshot으로 재구성할 수 있습니다. `membershipEpoch`은 commit 완료 증거가 아니라 승인 시 제안된 membership 값으로 취급해야 합니다.

canonical tail은 accepted case에 `spot`, `membershipEpoch`, `receiveChunkLimitBytes`만 둡니다. reservation token, payload budget, target node/owner generations, transfer ID, completion identity는 의도적으로 없습니다. [service-wire-v1.schema.json](../../../framework/runtime/protocol/service-wire-v1.schema.json#L2239)

### 3. C++가 canonical send에서 깨지는 정확한 지점

기존 JSON 경로는 다음의 연속 transaction입니다.

1. source가 `transfer_id`, completion high/low ID, deadline, source authority/fence를 설정한다.
2. private admission packet이 그 transfer ID와 completion ID를 target으로 보낸다.
3. target `actor_transfer_coordinator`가 transfer ID 아래에 source/target spot, deadline, completion IDs, app reply를 parked admission으로 저장한다.
4. source가 seal → `transfer_actor_out` → prepare → finalize/cutover → Core commit → target route publication → completion delivery를 수행한다.

근거는 [mesh_node_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp#L2407), [spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp#L6003), [actor_transfer_coordinator.hpp](../../../framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.hpp#L34), [mesh_node_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp#L2755)입니다.

canonical wire branch는 completion IDs를 만들고 canonical request를 보낸 뒤, accepted tail을 받으면 receive chunk limit을 기록하고 곧바로 `deliver_remote_actor_join()`을 호출합니다. `seal_remote_application_actor_join()`와 이후 prepare/finalize chain으로 들어가지 않습니다. [mesh_node_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp#L2583), [mesh_node_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp#L2639)

따라서 Bingo/TicTacToe 회귀의 직접 원인은 “deep completion delivery 실패”가 아니라 다음 세 가지 state 분리입니다.

- source `s->transfer_id`, source spot generation, source authority snapshot, deadline, seal state가 JSON 경로처럼 초기화·소비되지 않는다.
- target은 canonical correlation으로 파생한 `wire-actor-join:<source rid>:<source generation>:<correlation>` ID 아래 admission을 실제로 park한다. source는 그 identity를 알거나 후속 prepare에 사용하지 않는다. [mesh_node_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp#L517)
- target parked admission은 completion identity exact match를 요구한다. canonical receiver가 저장하는 `{source node generation, correlation}`와 source가 JSON 방식으로 만든 `{mesh hash, counter}`는 다르다. 그러므로 단순히 canonical send 뒤 JSON prepare를 이어도 target prepare identity가 맞지 않는다. [spot_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp#L6089)

또한 현재 HEAD에서는 canonical receiver의 cold target stable-type 조회는 authority row fallback으로 보완돼 있습니다. 과거 revert 설명의 “stable type을 못 찾는다”는 부분은 현 상태의 주 원인이 아닙니다. 실제 남은 핵심은 sender/target reservation identity와 completion identity의 통합입니다. [mesh_node_runtime.cpp](../../../framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp#L457)

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



---

## triple-at-28-mint 검증 (2026-08-21, advisor "붕괴" 가설 검증 — 두 질문 모두 NO)

**Q1 (coordinator가 28-mint시 triple 아는가): 4언어 모두 NO.**
- node: admission 전엔 relocationId만; coordinator/targetAttemptGeneration은 runCoordinator에서 40 구성시 처음 생성(service-relocation-host-runtime.ts:1924/1969). 28 mint(actor-local-native-join.ts:350)는 그 전.
- java: 28은 fence만으로 mint(ZLinkJavaRawMeshNode.java:1345); relocation goal은 28 응답 **후** 생성(ZLinkActorSpotJoinCall.java:817). 전체 identity는 sourcePrepare(ZLinkCanonicalRelocationStateMachine.java:977).
- dotnet: 28 mint=generic BeginJoin(ZLinkManagedMeshNode.cs:3054), triple은 CreatePrepare(40)에서 처음(ZLinkActorRemoteJoiner.cs:812). **게다가 hosted relocation-driven join은 JSON admission 사용, canonical-28 경로 아님**(ZLinkActorRemoteJoiner.cs:452).
- cpp: **relocation-driven canonical-28 live 경로 없음** — production은 JSON fallback(mesh_node_runtime.cpp:2317). triple은 coordinator에서 40 구성시(mesh_node_runtime.cpp:1176/1219).

**Q2 (optional trailing triple codec slot): clean slot 없음.** 현행 28 body는 Frame 0가 targetSpot에서 정확히 끝나야 하고 frame count 1..2, Frame 1은 application payload 전용, trailing byte reject. 유일 배치=Frame-0 presence-bool+triple(targetSpot 이후·Frame 1 이전) → schema/spec/generator/runtime 동기 개정 + capability/version gating 필요, 구decoder는 relocation-tail을 reject(하위호환 아님).

**결론: advisor의 "28이 triple을 upfront 선언 → 문제 붕괴" 경로 불성립.** 근본 순서가 admission-first→40-mint(triple 생성)이므로 28은 triple 존재 이전. 더욱이 dotnet/cpp는 relocation-driven join이 canonical 28을 아직 쓰지도 않음(JSON). 따라서 cpp/dotnet canonical 발신 완주 = (a) relocation-driven join을 JSON→canonical 28로 전환 + (b) relocation identity 배선 + (c) 바인딩 확립, 이 셋을 요하는 집중 통합. **first-40-pin(수신측 바인딩) 또는 wire 확장 둘 중 하나가 불가피** — advisor 재판단 요청 대상.


---

## node/java 생존 방식 검증 (2026-08-21, advisor branch A/B 판별 — narrow node read)

**정정: node/java는 relocation-driven join을 canonical 28로 통합했다(capability-gated).** sender submitActorJoin(service-stateful-runtime.ts:4246-4269)이 canUseCanonical시 encodeActorJoin28로 발신하고, canonical.local.transferId(=relocationId)는 **로컬 bookkeeping**일 뿐 wire body에 안 들어감(spec §9). 수신측은 wire 마커 부재→canonical decode(service-stateful-runtime.ts:1927-1930). 앞선 "node relocation=legacy only" 추론은 오류.

**branch A 성립: 28→40 바인딩 race가 구조적으로 도달불가.**
- canonical 수신 admission = admitActorJoin(spot-actor-membership.ts:54)이 activation serial turn 내 **evaluate→commit 동기 트랜잭션**. 나중 command 40을 기다리며 오래 park되는 free-floating 28 admission이 없음 → stale 40이 붙을 대상 부재.
- command 40 state rendezvous = 별도 트랙 TargetRelocationReservation(service-relocation-host-runtime.ts:2416), **source가 mint한 relocation-identity(triple) 키**. 28 admission map(actorJoinPhases, pending.id 키)을 참조하지 않음.
- 두 트랙은 commit시 actor fence로 만남. 28↔40 identity 바인딩 자체가 없으므로 "stale 40이 잘못된 28에 바인딩"하는 hole이 성립 불가.
- 같은 actor 2차 canonical 28은 activation serial queue에 직렬화(병렬 parked admission 생성 안 함). legacy routed-admission(spot-routed-actor-admission.ts)은 transferId 키·idempotent, 별개 경로.

**결론: 5-STOP 바인딩 saga는 실재하지 않는 hazard였다.** binding-at-first-40/28-carries-triple/새 attempt-lifecycle 레이어 모두 불필요. cpp/dotnet canonical 발신은 node/java 패턴(e2e 입증) 위에 **바인딩 spec ruling 없이** 구축 가능. advisor 확인 대상: branch A 확정 + 발신 슬라이스 재개 go.


---

## receiver 두 경로 확인 + overclaim 수정 (2026-08-21, advisor hole 해소)

advisor가 지적한 hole 2건 해소 (remote-actor-join-receiver.ts 전체 read):
- **receive (:43, legacy JSON)**: admitActorJoin(:106)의 commit closure(:110-113)는 동기 — setJoinedSpot + rollback 반환, command 40 state를 await하지 않음.
- **prepareCanonicalActorJoin (:138, canonical 28)**: 주석(:132-137) "정상 typed Spot admission 이전에 호출". admitActorJoin을 호출하지 않음 — 타입 resolution + getOrCreateActor + setNativeActorRef(prepare)만 수행. 실제 admission은 정상 경로. lingering parked admission 없음.

**방어가능한 결론(967ab22b0c의 "hazard does not exist" 과장 수정):**
- canonical 경로에 **28→40 identity 바인딩이 존재하지 않음**(코드로 확인). 40 state 복원은 relocation-identity 키 reservation의 별도 트랙, 28 admission은 spot membership 트랙. commit closure는 40을 await하지 않음. 따라서 "stale 40이 잘못된 28 admission에 바인딩"하는 hazard는 현 canonical 설계에 성립하지 않음.
- **단, multi-attempt re-park 경로는 미검증(test-half).** node relocation e2e가 same actor에 2차 attempt(later-attempt-wins re-park)를 실행하는 테스트를 아직 확인하지 못함 → branch B(잠복·미검증) 가능성 배제 불가. 이는 **node/java 대상 별도 checklist 항목**으로 파일(발신 작업 차단 아님).
- cpp/dotnet canonical 발신은 node/java 패턴(canonical 28=type resolution+정상 admission, transferId 로컬 전용·wire 무배치) 위에 **바인딩 spec ruling 없이** 구축 가능. 단 agent에게 "commit이 40 state를 await하지 않는다 = 동기 admission 형상"을 명시.


---

## .NET 발신 슬라이스 1차 시도 = 정직한 STOP (2026-08-21, 실질 설계질문)

.NET agent(codex terra high, 파일 변경 0)가 well-formed 설계 질문에서 STOP:
- .NET command 28 수신이 target actor를 **즉시 예약 생성**(ZLinkFrameworkRuntimeActors.cs:2678). 28 wire엔 correlation+fence만(abort/relocation-identity/deadline 없음, service-wire-v1.schema.json:7292). target abort는 command-40 relocation identity로만 주소지정(ZLinkStandaloneActorRelocationRuntime.cs:2454).
- => 28 accepted 후 40 전 source precommit 실패 시 source가 target 28 예약을 식별·정리 불가. 28→40 바인딩 금지 + JSON token abort 흉내 금지 상태에서 해결 불가.
- 3옵션: (1) canonical 28이 target 예약을 안 만들고 40 예약 트랙만 사용, (2) correlation 기반 canonical-attempt abort 신설, (3) target-local expiry/lease cleanup 허용.

**진단(내가 부과한 invariant 오류):** "pre-commit failure가 target reservation을 정리" invariant 자체가 바인딩을 요구하는 모순이었음. node 형상 확인 결과:
- node canonical 28 = canonicalActorJoinResolver→prepareCanonicalActorJoin(타입 resolution + getOrCreateActor + setNativeActorRef). relocation 예약을 만들지 않음.
- node target abort/cleanup(abortTargetReservation/abortTargetStage, service-relocation-host-runtime.ts:3382/3390)은 **40 트랙(relocation identity 키)**이 소유.
- 즉 node는 **option 1** 형상: 28은 타입+actor 보장만, relocation 예약은 40이 소유. abandonment시 정리할 28-예약 자체가 없음(source seal만 정리).

**잠정 ruling(advisor 확인 대상): option 1.** .NET의 :2678이 admission과 relocation-reservation을 융합한 것이 .NET 특정 불일치. 수정 = canonical 28 수신은 타입 resolution + actor 보장만, relocation 예약은 40 트랙 소유로 분리. 이러면 pre-commit invariant는 "source seal만 정리"로 축소, 28→40 바인딩 불요, branch A와 일관. (단 이번 세션 node 내부 2회 오판 이력 → 단정 전 검증 필요.)


---

## .NET 발신 v2 검증: 독립 재현 PASS + sol 리뷰 NOT-CLEAN 6건 (2026-08-21)

**내 독립 재현**: focused seam+relocation 39/39 PASS(agent 보고 일치). agent 전체 게이트 1782/3(3 sanctioned), Bingo·relocation-dotnet-java 통과 보고 — 단 이 통과들은 동작하는 하위경로만 커버(아래 sol이 미커버 경로 노출).

**codex sol(gpt-5.6-sol) 판정: NOT-CLEAN.** 바인딩 제거·seam abort 단순화는 clean 확인. 그러나:
1. **CRITICAL — canonical 28 수신 dispatch 부재.** ManagedMeshNode 수신 whitelist(IsAllowedInfrastructureControlCommand)가 ActorJoin 제외 → source가 28 보내도 target이 protocol error로 drop(:5326). PrepareCanonicalActorJoinAsync(:2678)는 ZLinkSpotActorJoinDispatcher:42에서만 호출돼 wire ingress 미연결. **내가 직접 확인함(whitelist에 ActorJoin 없음).** .NET→Java 하니스 통과는 Java 수신자라 이 갭 미노출.
2. **HIGH — 28 수용 시 즉시 app delivery + membership commit.** provisional secure 후 ordinary dispatcher(ZLinkSpotActorJoinDispatcher:85 app handler, :108 accepted commit)로 재진입 → membership publication + Location lifecycle(:530). ruling("타입 resolution + provisional secure만")과 충돌. **단 node도 유사할 가능성(ruling 과엄격 여부) — advisor 판정 필요.**
3. **HIGH — public Join completion identity/callback 유실.** canonical 분기가 empty recovery(:899) → generic maintenance completion(:1166) 선택 → nonzero public completion ID의 target OnJoinCompletedAsync 미전달.
4. **HIGH — entry 하드코딩 false**(TryJoinCanonicalActor:3223). JoinEntrySpot canonical join이 non-entry로 오인코딩.
5. **MEDIUM — bound-session을 admission 제외로 처리**(:535). ruling은 bound Session이 seal/route leg만 추가, admission 배제 아님.
6. **MEDIUM — production invariant 미테스트**(수신 dispatch/accept-time commit 부재/bound gating/entry bit/public completion).

**판정: 커밋 보류.** decouple(:2678)+바인딩 제거+seam abort는 clean이나 origination(발신+gate+수신 dispatch+completion)에 6건 집중. 다음: #2 ruling 충돌을 node 대조로 해소 후 advisor 우선순위 상담(fix-forward 전체 vs clean-subset 랜딩).


---

## 단계 1(A1) 검증 결과: sol NOT-CLEAN 7건 — 랜딩 보류 (2026-08-21)

**독립 검증**: 제 diff 정독(라우팅·fence·entry·u64는 정합)·빌드 0/0·focused 40/40 통과. 전체
게이트 1803/4 = sanctioned 3 + RelocationBehaviorConformanceTests.ActorJoin_target_ready_submit_
failure_reuses_staging(격리 통과 → 전체-실행 flake, 기존 클래스 잠복위험). **그러나 sol NOT-CLEAN.**

**sol 7건**:
1. [HIGH] 28이 40 전에 membership commit(ZLinkSpotActorJoinDispatcher.cs:115→ZLinkSpotActivationActors.cs:530
   NotifyActorJoinedSpotAsync) — §9 provisional-secure-only 경계 위반(relocation-28의 경우).
   **재발 tension**(sender review #2와 동일) — 내 §9 ruling과 충돌, advisor 판정 필요.
2. [HIGH] mailbox full시 28 silent drop(ZLinkManagedMeshNode.cs:5474 EnqueueOwned bool 무시 →:9313
   dispose+local backpressure event만, typed terminal 없음 → source timeout).
3. [HIGH] whitespace Actor ID가 canonical로 claim(ZLinkServiceWireCodec.cs:739 empty만 거부)→
   FromBoundary throw→broad catch가 node를 Error 상태로(:4625), source에 ProtocolError 미전달.
4. [HIGH] 기존 local Actor가 stable-type 검증 우회(ZLinkFrameworkRuntimeActors.cs:2699 state.Actor
   존재 시 즉시 return, state.ActorType==stableType 미검증) → type A/B 혼동.
5. [MED] TypeMismatch를 ProtocolError로 인코딩(dispatcher:345) — schema는 conflict+actorTypeMismatch 제공.
6. [MED] terminal-send backpressure가 Store 실패를 application rejection으로 변환(dispatcher:52→56).
7. [MED] 신규 테스트가 §9 Store admission 우회(ReplyJoin 수동 호출, StatefulServiceRuntimeTests:1694)
   → #1/#4/#5/#6 미탐지. **내 "진짜 wire 테스트" 평가가 이 점에서 오류**(wire 전송은 타나 Store
   admission 경로는 short-circuit).

**판정: 랜딩 보류(diff 미커밋 유지).** land/fix-forward는 #1 §9 tension 해소 후 결정. #1 핵심질문:
canonical 28 수신이 membership commit해야 하나(plain join은 정답) vs relocation-28은 40까지 defer해야
하나 — 수신자가 둘을 구분하는가.


---

## 단계 1(A1) fix-forward: #2-#6 적용, #1에서 정직 STOP (2026-08-21)

sol 7건 중 **#2·#3·#4·#5·#6 적용 완료**(focused 40/40):
- #3 malformed Actor ID: 공백/NUL을 canonical decode에서 거부 + 경계에서 ProtocolError terminal.
- #2 mailbox-full: enqueue 실패 시 Backpressured command-20 terminal.
- #4 existing Actor: Store stableType vs 기존 ActorType 불일치를 TypeMismatch 거절.
- #5 TypeMismatch → Conflict/actorTypeMismatch wire 매핑.
- #6 typed Store terminal backpressure: application rejection 대신 원 terminal 재시도.

**#1에서 STOP(정직) — 제 §9 명확화의 허점을 노출**: command 28엔 relocation-여부/identity가 없고
28↔40 결합 금지 → target이 plain vs relocation을 구분 불가. "모두 moving admit + plain 즉시 해제"는
순환(해제가 plain 식별을 전제). 게다가 node 1차 증거와 충돌: node canonical 28은 beginMove 미호출,
moving은 40-track(actor-transfer-runtime:403/702)이 설정. → 제 "28에서 moving" 명확화 재검토 필요.
**#1·#7(real-dispatcher 테스트)는 이 discriminator 스펙 판정 후 진행.** diff 미커밋 유지.
