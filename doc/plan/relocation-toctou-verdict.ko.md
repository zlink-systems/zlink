# coordinator TOCTOU 판정 보고서 (B-2 실존 — cpp·java 잠재 결함)

> 작성: codex gpt-5.6-sol(ultra) 조사 에이전트, 검수: Claude 감독관 (2026-08-30).
> 결론: 재검증 제거 불가이며, 현행 cpp 재검증도 실제 창을 못 막는다 — membership :903 위반.
> 수정 방향: coordinator admission과 Actor FIFO admission을 원자적으로 묶는 fence(cpp), post-cut 거부 결과 처리(java).

# 판정

**제거 불가 — B-2가 실존한다.**

다만 중요한 정정이 있다.

- 사용자가 제시한 문자 그대로의 순서인 `capture 완료 → materialize/enqueue`는 canonical C++ remote/stateful ingress에서는 성립하지 않는다. ingress가 Core application claim을 잡고 있어 capture가 먼저 끝날 수 없다.
- 실제 가능한 순서는 `barrier 예약 → barrier 뒤 enqueue → Core claim 해제 → capture/commit → old-source handler 실행`이다.
- 이 경로는 현재의 10862 재검증도 막지 못한다. 따라서 “재검증을 유지하면 안전”도 아니고, “relocation 장치가 대신 처리하므로 삭제해도 안전”도 아니다. **단순 삭제가 아니라 coordinator admission과 Actor FIFO admission을 원자적으로 묶는 fence로 교체해야 한다.**

## C++ 시나리오 추적

### 1. 사용자가 지정한 literal ordering은 불가능

원격 record는 Framework dispatch보다 먼저 같은 Actor의 Core application turn을 claim한다.

- host가 record를 claim: [public_host_runtime.cpp:5821](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:5821)
- `raw_stateful_dispatch`가 object claim 획득: [raw_stateful_dispatch.cpp:654](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/raw_stateful_dispatch.cpp:654)
- claim은 `application_active=true`: [stateful_object_runtime.cpp:890](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:890)

Relocation seal은 같은 object를 `moving`으로 바꾼 뒤 [stateful_object_runtime.cpp:1225](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1225), `application_active`가 사라질 때까지 기다린다 [stateful_object_runtime.cpp:1264](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1264). 실제 state/pending capture는 그 뒤다 [stateful_object_runtime.cpp:1296](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1296), [stateful_object_runtime.cpp:1345](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:1345).

one-way claim은 Framework Actor FIFO post 성공 뒤에야 해제된다.

- one-way terminal 생성: [spot_runtime.cpp:13635](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:13635)
- FIFO post 성공 뒤 terminal 실행: [spot_runtime.cpp:4281](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:4281)
- 최종 `application_active=false`: [stateful_object_runtime.cpp:911](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:911)

따라서 `capture 완료 → 그 뒤 enqueue`는 이 path에서 불가능하다.

### 2. 실제 가능한 B-2 순서

1. M이 coordinator를 한 번 조회하고 `not_moving`을 받는다.

   - 유일한 production admission call: [spot_runtime.cpp:10677](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10677)
   - move가 없으면 `not_moving`: [actor_transfer_coordinator.cpp:194](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.cpp:194)

2. 현재 Actor handler가 source를 reserve하고 Actor FIFO에 application-lane barrier를 넣는다.

   - source reservation: [spot_runtime.cpp:7965](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:7965)
   - barrier reservation: [spot_runtime.cpp:7985](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:7985)
   - 실제 application-lane queue record: [serial_execution_queue.cpp:667](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.cpp:667)

3. M은 source route/context가 아직 current이므로 warm materialization을 통과한다.

   - materialization wait: [spot_runtime.cpp:10862](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10862)
   - claim은 relocation 상태가 아니라 동일 Spot context만 확인: [spot_runtime.cpp:10835](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10835), [spot_runtime.cpp:11048](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:11048)

4. M이 barrier 뒤 Actor FIFO에 old-source `spot`/`actor` 포인터를 가진 closure로 들어간다.

   - handler enqueue: [spot_runtime.cpp:11158](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:11158)
   - 실제 queue post: [spot_runtime.cpp:4120](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:4120)
   - 이 호출의 `before_invoke`는 빈 callback: [spot_runtime.cpp:11162](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:11162)

5. post 성공으로 Core claim이 풀린다. 이제 relocation seal/capture가 진행된다.

   이때 M은:

   - coordinator `_backlogs`에 없다. 최초 `not_moving` 뒤 재호출이 없기 때문이다.
   - Core `queue.application`에도 없다. 이미 Core에서 claim되어 Framework FIFO로 빠져나갔기 때문이다.
   - `held_application`에도 없다. `moving` 이후 새로 Core에 enqueue된 record만 여기 들어간다 [stateful_object_runtime.cpp:811](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:811), [stateful_object_runtime.cpp:832](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/stateful_object_runtime.cpp:832).

6. commit은 Message Follow를 등록하고 coordinator backlog만 drain한다.

   - Message Follow 등록과 target route 게시: [spot_runtime.cpp:8712](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:8712)
   - late replay loop: [spot_runtime.cpp:8776](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:8776)
   - `finish_move_replay`가 가져가는 것은 `_backlogs`뿐: [actor_transfer_coordinator.cpp:165](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.cpp:165)

7. Join terminal 뒤 barrier가 풀리면 정상 Actor queue drain이 M을 줍는다.

   - barrier는 async relocation completion까지 turn을 보유: [spot_runtime.cpp:557](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:557)
   - 일반 queue drain: [serial_execution_queue.cpp:1015](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.cpp:1015)
   - dequeue 시 relocation/stale 재검증 없이 캡처한 old-source handler 직접 호출: [spot_runtime.cpp:4166](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:4166)

결과는 물리적 drop이 아니라 더 위험한 **조용한 wrong-owner 실행**이다. one-way는 이미 FIFO admission 성공으로 완료됐지만, handler의 mutation은 snapshot 이후 old source에만 적용되고 target state에는 없다. 권위 state 관점에서는 operation이 유실된다.

request는 Core claim이 handler terminal까지 유지되므로, relocation seal은 claim을 기다리고 handler는 barrier 뒤에서 기다리는 상호대기/timeout 후보다.

## 왜 backlog·Message Follow·stale retry가 구제하지 못하는가

- Coordinator backlog에는 `try_append_backlog_unlocked`가 실제 push한 packet만 들어간다 [actor_transfer_coordinator.cpp:190](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.cpp:190), [actor_transfer_coordinator.cpp:232](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.cpp:232). M은 그 전에 `not_moving`으로 빠졌다.
- Message Follow는 commit 뒤 등록되며, 새 ingress가 다시 `admit_dispatch_packet`을 통과할 때만 선택된다. 이미 Framework FIFO closure가 된 M은 그 경로에 재진입하지 않는다.
- source cleanup은 Actor instance map만 지우며 FIFO를 scan/migrate하지 않는다 [spot_runtime.cpp:6054](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6054). coroutine이 `shared_ptr`를 보유하므로 queued old handler는 생존한다 [spot_runtime.cpp:10911](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10911).
- request sender가 자동 재시도하는 것은 `actor_transfer_in_progress` origin뿐이다 [actor_client.cpp:645](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:645). 그 origin은 coordinator 이동 판정에서만 만들어진다 [spot_runtime.cpp:10725](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10725). 이 B-2에는 stale terminal 자체가 없다.
- one-way sender는 한 번 resolve/submit하고 끝난다 [actor_client.cpp:555](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:555).
- `actor_gateway_runtime` 전체에는 Actor transfer coordinator 재검증이 없다. session relocation route와 session fence만 있으며, relay failure는 `drop`으로 보고한다 [actor_gateway_runtime.cpp:328](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:328).

## 언어별 대조

| 언어 | 같은 위치의 보호 | barrier/seal 이후 ingress 처리 | 판정 |
|---|---|---|---|
| C++ | `inherit_materialized_context`/`protects_context`가 있으나 Spot context 동일성만 확인 | Core queue, coordinator backlog, Framework Actor FIFO가 분리되어 그 사이에 M이 빠짐 | B-2 존재. 더 강한 atomic fence 필요 |
| .NET | 의미상 동일한 dispatch 직전 재검증이 있음. detached queue 뒤 registry를 다시 읽고 `ProjectDispatchIngress` 실행 [ZLinkActorInboundPipeline.cs:308](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkActorInboundPipeline.cs:308), [ZLinkActorHandoffState.cs:477](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:477) | 같은 state lane에서 sealed arrival를 `_sourceHoldFrames`로 capture [ZLinkActorHandoffState.cs:437](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:437); freeze 뒤 suffix도 Message Follow 전환 시 반환 [ZLinkActorHandoffState.cs:1117](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:1117) | 해당 TOCTOU가 닫힘 |
| Node | 보편적 late claim은 없음 | `capture()` 결과 확인부터 direct dispatch/FIFO push까지 await가 없는 같은 JS turn [spot-actor-admission-coordinator.ts:52](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/spots/spot-actor-admission-coordinator.ts:52), [serial-execution-queue.ts:282](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/execution/serial-execution-queue.ts:282). 먼저 handoff가 시작되면 durable pending으로 capture되고 [actor-handoff.ts:827](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-handoff.ts:827), cutover가 active 삭제와 MF 설치를 같은 turn에서 수행 [actor-handoff.ts:1023](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-handoff.ts:1023) | 구조적으로 동일 interleaving 불가 |
| Java | C++식 late current-context claim 없음 | forward 조회 [ZLinkJavaRawSpotNode.java:1880](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:1880) → authority 검사 [ZLinkJavaRawSpotNode.java:1917](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:1917) → enqueue [ZLinkJavaRawSpotNode.java:2043](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:2043) 사이 창이 있음. post-cut queue는 `queue owner has relocated`로 거부하지만 [ZLinkSerialExecutionQueue.java:356](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java:356), caller가 반환 stage를 버리고 `true`를 반환 | Java에도 별도 B-2 존재. Java의 부재는 C++ 삭제 근거가 아님 |

즉 C++만의 특별한 “재검증 불필요” 구조가 있는 것이 아니다. .NET은 실제 재투영을 하고, Node는 동기 선형화로 창 자체를 없앤다. Java는 재검증 부재 때문에 같은 결함이 보인다.

## 스펙 판정

스펙은 침묵하지 않는다.

가장 직접적인 조문은 membership contract test 요구다.

> “`Defer()` 뒤 source seal 전 message는 barrier 뒤 Actor queue에 두고, seal 뒤 message만 relocation ingress hold에 보관한다.”  
> — [05-spot-actor-membership.ko.md:903](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/05-spot-actor-membership.ko.md:903)

실제 C++ M은 바로 **Defer 이후, seal 이전, barrier 뒤 Actor queue** record다. 따라서 seal 때 saved Actor work로 capture되어야 한다. 현재 Core capture가 Framework FIFO record를 보지 못하는 것이 계약 위반이다.

Routing도 명시적이다.

> seal 전 수락한 작업은 이전 queue/accepted journal에 포함하고, seal 뒤 source arrival는 ingress hold에 보관한다.  
> — [08-routing.ko.md:222](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:222)

> caller가 새 route를 선택하거나 operation을 다시 만들 필요가 없다.  
> — [08-routing.ko.md:240](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:240)

반대로 실패한 현재 operation의 자동 재제출은 금지한다 [08-routing.ko.md:244](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:244). 따라서 “stale이면 sender가 어차피 재전송한다”는 일반 불변식은 없다.

Relocation flow 역시:

- 이미 수락했지만 실행하지 않은 작업을 capture: [04-relocation-flow.ko.md:108](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:108)
- 새 source ingress는 hold: [04-relocation-flow.ko.md:155](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:155)
- cutover 동안 새 arrival는 boundary 뒤 구간: [04-relocation-flow.ko.md:208](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:208)
- saved work → relay → temporary queue 순서와 atomic ordinary-ingress 개방: [04-relocation-flow.ko.md:298](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:298)
- lock/actor-loop/executor 선택만 언어 재량이고 전이 순서는 재량이 아님: [04-relocation-flow.ko.md:616](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:616)

따라서 계약은 정상 same-generation ingress에 대해 **capture/hold/temp/MF 보존**을 요구한다. stale 거부는 invalid·expired route의 terminal일 뿐, 이미 수락한 pre-seal operation을 조용히 잃는 대체 수단이 아니다.

## 전체 검색으로 확인한 부재

```text
rg -n 'admit_dispatch_packet|try_append_backlog|finish_move_replay|reserve_handoff_barrier|inherit_materialized_context|protects_context' .

rg -n '(take_backlog|finish_move_replay|complete_move_and_take_backlog|actor_queue|erase_actor_queue|replace_actor_queue|queue_identity|pending_count)' \
  framework/languages/cpp/framework/src/runtime/{spots,actors,mesh} --glob '*.{cpp,hpp}'

rg -n 'actor_transfer_coordinator|admit_dispatch_packet|try_append_backlog|finish_move_replay|Message Follow|message_follow|relocation|handoff' \
  framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.{cpp,hpp}

rg -n -i 'not_moving|materializ.*(?:barrier|handoff)|(?:barrier|handoff).*materializ|barrier.*enqueue|enqueue.*barrier' \
  framework/languages/cpp/tests
```

결과:

- production `admit_dispatch_packet` 호출은 `spot_runtime.cpp:10677` 한 곳뿐이다.
- remote transfer source에서 Framework Actor FIFO를 capture/migrate하는 경로는 없다.
- gateway 결과는 session relocation뿐이며 Actor coordinator 재검증은 없다.
- 정확한 `not_moving → barrier reserve → barrier 뒤 enqueue` 회귀 test도 없다.

## [의심]

- **[H] 현재 코드도 이미 불안전하다.** 10862 wait는 coordinator fence가 아니므로 위 one-way B-2를 닫지 못한다.
- **[H] request 변형은 seal과 barrier 뒤 handler 사이의 상호대기 후 timeout으로 귀결될 가능성이 높다.**
- **[M] Java에도 독립적인 post-cut enqueue B-2가 있다.**
- **[M] Node는 이 TOCTOU는 닫지만, one-way Message Follow에서 `sendToSpot`의 non-`Submitted` 반환 status를 검사하지 않는 별도 quiet-loss 후보가 있다 [actor-handoff.ts:1531](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-handoff.ts:1531).
- **[M] 10862 계열 검사는 local/non-stateful direct path의 context 교체도 보호한다. remote canonical path만으로 전역 삭제 안전성을 확장할 수 없다.**

변경 파일은 없으며 git, 빌드, 테스트는 실행하지 않았다. 판정은 순수 정적 코드·스펙 추적 결과다.


