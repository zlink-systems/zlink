# Node DeliveryDispatch `courier-b` NotFound 진단

## 결론

실패를 만든 판정 주체는 `courier-node-1`이 아니라 **dispatch process의 Node Framework
direct Actor resolver**다. Dispatch는 `courier-b`의 positive Ready route를 아직 cache하지 않은
상태에서 Location Store를 읽었다. 직전 `GetOrCreate`는 같은 ID의 active authority를 확인해
`ActorRef`를 반환했지만, 이어진 direct resolve는 usable owner lease를 얻지 못해 route를 만들지
않았다. Node는 이 상태를 `undefined`로 축약하고 `ActorRouteNotFound`로 바꿨다
(`framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts:388-411`,
`framework/languages/node/packages/framework/src/runtime/actors/actor-client.ts:301-319`). 이 internal
kind는 public `NotFound`다
(`framework/languages/node/packages/framework/src/runtime/framework-errors-internal.ts:53-75`).

따라서 관찰 결과는 다음과 같다.

| 후보 | 판정 | 근거 |
|---|---|---|
| authority snapshot 자체가 없음 | 아님 | 같은 `startOffer`의 직전 `GetOrCreate`가 active authority가 아니면 existing `ActorRef`를 반환할 수 없다. |
| dispatch의 positive route cache에 `courier-b`가 없음 | 맞음 | 이 실행에서 dispatch가 `courier-b`에 보내는 첫 direct call이다. Bind 경로는 courier-session process의 별도 binding route다. |
| relocation 뒤 stale route cache | 아님 | cache miss 경로이며 Courier node는 relocation을 끈다. 실패 실행과 8회 재현 모두 relocation debug event가 없었다. |
| binding fence generation 불일치 | 아님 | 실패는 frame 생성과 transport submit보다 앞이다. `courier-node-1`의 binding/admission 검증은 실행되지 않았다. |
| active authority의 owner lease가 usable하지 않음 | 증거와 일치하는 분기 | active snapshot을 확인한 직후 direct resolver가 route를 만들지 않는 남은 정상 분기는 `remainingOwnerTokenLeaseMs(...) <= 0`이다. Authority는 lease 만료로 자동 삭제되지 않는다. |

이것은 **B — 기존 Node Framework 결함**이다. `08-routing`이 요구하는 `Missing`과
`Unavailable` 구분을 Node direct Actor resolver가 잃는다. Sample이나 상위 Framework가 Core나
binding의 결정을 다시 구현한 문제는 아니다. Source Framework가 소유한 올바른 판정 위치에서
결과 종류를 잘못 축약한 문제다.

다만 이 진단으로 “왜 그 한 실행에서 `courier-node-1`의 lease 갱신이 usable하지 않았는가”까지
확정할 수는 없다. 보존된 실패 실행에는 owner-lease event/metric sink가 없고, 현재 flow tracer도
direct resolve 이전 실패를 기록하지 않는다. 격리 재현 8회는 모두 통과했다. 따라서 lease 갱신
실패의 선행 원인을 추측해 heartbeat, timeout 또는 retry를 바꾸면 안 된다. 이번 단계에서 확정한
결함은 **owner-unavailable을 NotFound로 오분류한 경계**다.

## 재현성과 수집 조건

모든 재현은 `framework/languages/node`에서 아래 조건으로 실행했다. Core와 local package는 다시
만들지 않았다.

```bash
env -u ZLINK_LIBRARY_PATH \
  TMPDIR=/dev/shm/zlink-tmp-node \
  ZLINK_DEBUG_FRAMEWORK_RELOCATION=1 \
  flock -w7200 /tmp/zlink-node-gate.lock \
  bash samples/run_samples.sh DeliveryDispatch.Ts
```

DeliveryDispatch의 다섯 server module은 이미 `messageFlow('normal')`을 설정한다
(`framework/languages/node/samples/DeliveryDispatch.Ts/Server/Courier/courier-module.ts:39`,
`framework/languages/node/samples/DeliveryDispatch.Ts/Server/CourierSession/courier-session-module.ts:30`,
`framework/languages/node/samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-center-module.ts:32`,
`framework/languages/node/samples/DeliveryDispatch.Ts/Server/Session/session-module.ts:34`,
`framework/languages/node/samples/DeliveryDispatch.Ts/Server/Tracking/tracking-module.ts:31`). 첫 재현부터 임시 OTel file sink를 연결했고 8회가 끝난 뒤
제거했다. Selector runner는 성공한 run directory를 삭제한다
(`framework/languages/node/samples/run-sample.mjs:42-67`). 실패가 없었으므로 이 8회의 flow file은
남지 않았다. 이전 gate의 실패 directory는 실패 시 자동 보존된
`/dev/shm/zlink-tmp-node/zlink-deliverydispatch.ts-SBE77b`다.

| 실행 | 조건 | 결과 | 경과 시간 |
|---:|---|---|---:|
| 이전 표준 `npm test` gate | 전체 gate, `sample-regression.test.js` 48번 | 실패, preserved run `SBE77b` | sample test 약 60.1 s |
| focused 1 | 위 명령 | pass | 7.47 s |
| focused 2 | 위 명령 | pass | 8.30 s |
| focused 3 | 위 명령 | pass | 8.05 s |
| focused 4 | 위 명령 | pass | 7.96 s |
| focused 5 | 위 명령 | pass | 8.27 s |
| focused 6 | 위 명령 | pass | 9.45 s |
| focused 7 | 위 명령 | pass | 8.77 s |
| focused 8 | 위 명령 | pass | 12.56 s |

Focused 결과는 8 pass / 0 fail이다. 표준 gate의 실패 사실과 원문은
`doc/plan/c016-worklog/stage1-node-gate-integrity-summary.md:36-45,72-90`에도 기록돼 있다.
간헐성은 확인됐지만 격리 실행에서 다시 만들지는 못했다.

## 실패 실행 timeline

### 먼저 바로잡을 wire command

이 sample의 bind relay는 command 36/33 조합이 아니다. 현재 canonical binding 계약의 실제
순서는 다음과 같다
(`framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:144-177,186-214,287-298`).

1. Session owner가 Actor owner에 `boundSessionBind(38)` request를 보내 binding을 등록한다.
2. Bind가 완료된 뒤 session payload를 `actorSend(24)`로 Actor application queue에 relay한다.
3. Offer handler가 실행되면 Actor가 bound session으로 보내는 push가
   `boundSessionSend(36)`이다.

Command 33은 maintenance용 `replyRelay`다
(`framework/runtime/protocol/service-wire-v1.schema.json:7457-7488`,
`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-wire-codec.ts:1800-1811`).
Actor relocation을 끈 이 one-way offer 경로에는 command 33이 없다. 실패한 `courier-b` offer는
Actor owner에 도착하지 않았으므로 command 36도 발생하지 않았다.

### 시간순 증거

아래 절대 시각은 preserved run의 file mtime과 evidence의 epoch millisecond를 함께 사용했다.
File log는 line별 timestamp를 넣지 않으므로 bind 구간의 millisecond 순서는 handler 의미로
보완했다.

| 시각 (2026-09-05 KST) | 단계 | 관찰과 판정 |
|---|---|---|
| 09:14:51.895–51.930 | `courier-a`, `courier-b` bind | `courier-session.log:2-3`에 두 binding 완료, `courier-node-1.log:2`에 `bind-relayed courier=courier-b`가 있다. Node sample은 `getOrCreate → bindOrGet → actor.relay` 순서다 (`framework/languages/node/samples/DeliveryDispatch.Ts/Server/CourierSession/courier-session.ts:38-54`). 따라서 command 38 terminal 뒤 command 24가 `courier-node-1` application handler에 admission됐고, `framework/languages/node/samples/DeliveryDispatch.Ts/Server/Courier/courier-actor.ts:17-22`가 응답했다. |
| 09:14:52.079 | 두 번째 delivery 첫 제안 | `deliverydispatch-evidence.jsonl:5`에 `delivery-reassign / Assigned / courier-a`가 기록됐다. |
| 09:14:58.108 | 재배차 시작 | 첫 제안 뒤 6,029 ms에 `Reassigned / courier-b`가 기록됐다 (`deliverydispatch-evidence.jsonl:6`). 설정은 renew 1,000 ms, TTL 5,000 ms, fencing margin 500 ms다 (`framework/languages/node/samples/DeliveryDispatch.Ts/Server/Configuration/location-store.ts:14-20`). 긴 지연이 갱신 실패 시 lease fence를 실제로 드러낼 수 있는 구간을 만들었지만, deadline을 늘릴 근거는 아니다. |
| 09:14:58.108 직후 | placement 조회 | `DispatchWorker.startOffer`는 먼저 `findOrEnsureActor`를 기다린다 (`framework/languages/node/samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-worker.ts:52-68,124-133`). `GetOrCreate`의 placement coordinator는 Store `reserve` 결과에서 active Actor만 existing으로 반환한다 (`framework/languages/node/packages/framework/src/runtime/host/actor-placement-coordinator.ts:109-129,358-379`). 이어 `Reassigned` publish가 성공했으므로 이 단계가 끝났다. |
| 09:14:58.108 직후 | direct resolve | Offer row를 저장한 뒤 public `sendToActor(actor.actorId, ...)`를 제출했다 (`framework/languages/node/samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-worker.ts:69-83`). Dispatch에는 `courier-b` direct cache entry가 없으므로 `directActorRoutes` miss 뒤 authority와 owner lease를 읽었다 (`framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts:170-172,388-411`). Bind route는 다른 process가 가진 별도 상태라 이 cache를 채우지 않는다. |
| 같은 call | dispatch 없음 | `remainingOwnerTokenLeaseMs`가 usable route를 주지 못한 경로에서 resolver가 `undefined`를 반환했고, actor client가 정확한 로그 문자열인 `Actor route 'courier-b' was not found.`를 만들었다 (`framework/languages/node/packages/framework/src/runtime/actors/actor-client.ts:312-317`). Route resolve가 frame 생성보다 먼저다 (`framework/languages/node/packages/framework/src/runtime/actors/actor-client.ts:119-130`). 따라서 actor frame 생성, route transport, `courier-node-1` target admission과 Actor handler dispatch는 모두 0회다. |
| 같은 call | local terminal | `dispatch.log:8`의 문자열은 위 source 코드가 만드는 문자열과 일치한다. `ActorRouteNotFound → public NotFound` mapping도 source process 안에서 끝난다. 이것은 remote request reply가 아니라 public one-way Actor send의 rejected completion이다. |
| 09:14:58.813 | sample 후속 실패 | 전송 전에 저장한 두 번째 offer의 deadline이 58.813이고 최종 상태가 `Failed`다 (`delivery-offers.json:13-20`). 다음 sweep이 후보를 소진시켜 `deliverydispatch-evidence.jsonl:7`과 `dispatch.log:9`를 남겼다. |
| 09:15:19.144 | browser 종료 | 기대하던 `courier-b` push와 상태열이 없어서 browser가 `Operation canceled`와 out-of-sequence를 보고했다 (`browser-client.log:1-20`). 이는 앞선 local NotFound의 후속 증상이다. |

실패 run에 flow file이 없는 이유도 중요하다. `messageFlow('normal')` 설정만 있고 당시 file
provider가 연결되지 않았다. 더구나 actor client는 route를 얻은 뒤에야 message parts를 만든다.
이번 오류는 그보다 앞에서 발생하므로 현재 source에는 resolve miss의 `message_flow_outcome=error`
기록 지점도 없다. 따라서 실패 timeline의 resolve와 local terminal은 preserved application log,
유일한 오류 문자열, 호출 순서와 Store state 전제의 결합으로 판정했다. Target dispatch가 없다는
것은 추측이 아니라 `framework/languages/node/packages/framework/src/runtime/actors/actor-client.ts:127-130`의 선행 실패 경계다.

## Location Store와 route cache 판정

Authority record는 owner lease가 끝나도 자동 만료되지 않는다
(`05-location-relocation/01-location-runtime.ko.md:264-286,314-354`). `GetOrCreate`가 active
authority를 확인한 직후 lifecycle operation 없이 같은 ID가 Missing이 되는 정상 경로는 없다.
Sample은 Courier Actor relocation도 끈다
(`framework/languages/node/samples/DeliveryDispatch.Ts/Server/Courier/courier-module.ts:45-53`). 따라서
실패 시점의 상태는 다음처럼 좁혀진다.

```text
dispatch GetOrCreate
  -> Store active authority 확인, existing ActorRef 반환
  -> Reassigned 상태 publish
  -> dispatch-local direct cache miss
  -> 같은 authority 조회
  -> owner lease admission lifetime 없음/만료
  -> Node resolver가 undefined 반환
  -> Node actor client가 local NotFound 생성
  -> courier-node-1에는 offer 미제출
```

`ZLinkOwnerLeaseTracker`는 100 ms보다 오래됐거나 live가 아닌 snapshot을 그대로 재사용하지 않고
Store를 다시 읽는다
(`framework/languages/node/packages/framework/src/runtime/locations/lease-tracker.ts:93-176`). 그러므로 6초 된 positive lease
snapshot을 무조건 재사용한 결과도 아니다. 문제의 route cache는 stale relocation cache가 아니라
**아직 entry가 없는 cache**였고, Store read 뒤 owner lease admission에서 ReadyRoute 생성을
거부했다.

Location runtime은 1초마다 갱신을 예약하고, local deadline을 넘으면 owner를 unhealthy로
바꾼다
(`framework/languages/node/packages/framework/src/runtime/locations/runtime.ts:390-441,496-510,1008-1039`). 보존 로그에는 갱신 성공/실패
event가 없어서 다음 중 어느 선행 사건이었는지는 구분할 수 없다.

- Redis renew가 실패하거나 500 ms timeout을 넘었음
- `courier-node-1` event loop가 heartbeat를 TTL/fence 뒤까지 실행하지 못함
- Store에서 같은 owner token을 더는 current로 인정하지 않음

이 셋은 모두 resolver 관점에서 `Unavailable`이며 `Missing`이 아니다. 어느 하나를 골라 timer,
TTL이나 retry를 바꾸는 것은 현재 증거로 허용되지 않는다.

## 소유 계층과 spec 조항

| 결정 | 소유 계층 | 계약 | 현재 구현 판정 |
|---|---|---|---|
| Global ActorId를 current owner route로 바꿈 | Source Framework routing/location resolver | `08-routing` §2.1: cache miss면 Store를 읽고 Ready authority와 fence로 route를 만든다 (`08-routing.ko.md:57-98`). | 올바른 소유 위치다. Core/binding 결정을 복제하지 않는다. |
| Resolver 결과 종류 | Source Framework routing/location resolver | `08-routing` §2.2는 `ReadyRoute / Missing / Unavailable / StoreFailure` 네 닫힌 결과를 요구하고, authority가 있으나 owner를 쓸 수 없으면 `Unavailable`이라 한다 (`08-routing.ko.md:119-153`). | Node가 active owner lease 실패를 `undefined`로 줄여 이 계약을 위반한다. |
| Positive cache 수명과 invalidation | Source Framework routing/location resolver | Cache는 owner admission deadline을 넘지 못하고 owner lease invalidation 시 제거한다 (`08-routing.ko.md:119-128`; location runtime §7.3 `01-location-runtime.ko.md:828-845`). | Cache miss 및 lease 재검증 자체는 맞다. 실패 종류만 잘못됐다. |
| Owner가 새 작업을 받을 수 있는지 | Location runtime + Store owner lease | Store 시각과 monotonic 시각으로 local admission deadline을 계산하고, 만료/host identity 불일치면 Actor message 시작을 막는다 (`01-location-runtime.ko.md:561-599`). | Lease 때문에 route를 막는 결정은 맞다. 이를 NotFound로 바꾸면 안 된다. |
| Bind와 session relay | Session owner와 Actor owner | Session owner는 binding route/token/generation을 보관하고 Actor owner는 bind를 검증한다. Relocation runtime만 Store와 target 선택을 소유한다 (`02-session-actor-binding.ko.md:54-77`). Bind는 38, payload relay는 24, push는 36이다 (`:157-214,287-298`). | Binding은 이미 완료됐다. Direct Actor resolve가 binding 상태를 읽거나 재검증하지 않았고, binding fence mismatch도 아니다. |
| Relocation route switch | Relocation runtime, 이후 Session owner의 binding route 적용 | Direct cache, binding route와 Message Follow의 소유권은 분리된다 (`08-routing.ko.md:337-409`). | Relocation이 없었고 cache hit도 아니므로 해당 경로는 실행되지 않았다. |

Framework가 하위 계층 결정을 다시 구현하거나 같은 사실을 둘로 유지하는 site는 아니다.
Location Store는 authority와 lease 사실을 제공하고, public terminal 종류를 정하는 책임은 source
Framework resolver/mapper에 있다. `GetOrCreate`와 direct send가 연속해서 Store를 읽는 것도 서로
다른 공개 operation의 정식 경로다. 문제는 두 번째 경로가 첫 번째 경로의 결과를 복제한 것이
아니라, 자신의 `Unavailable` 결과를 `Missing`과 같은 값으로 표현한 데 있다.

## C++ / .NET / Java parity

세 구현 모두 client가 `courier-a`, `courier-b`를 먼저 bind한 뒤 첫 courier의 timeout 후
`courier-b`로 one-way Actor offer를 보낸다는 논리 순서를 갖는다. Node와 달리 .NET/Java의
dispatch worker는 offer마다 `GetOrCreate`를 한 번 더 하지 않고 ActorId direct send를 바로 한다.
C++는 `ActorDirectory.Find` 뒤 ActorId send를 한다. 이 구조 차이는 Node의 오분류를 정당화하지
않는다. 세 runtime 모두 owner-unavailable을 Missing/NotFound와 구분한다.

| 언어 | Sample의 bind → second offer | Runtime의 unavailable 처리 | 이전 gate |
|---|---|---|---|
| C++ | Courier session이 `get_or_create → bind_or_get → relay_request`를 수행한다 (`framework/languages/cpp/samples/DeliveryDispatch/Server/CourierSession/main.cpp:39-63`). Dispatch는 directory에서 ID를 찾고 public Actor send를 한다 (`framework/languages/cpp/samples/DeliveryDispatch/Server/Dispatch/main.cpp:139-160,230-246`). | Direct actor client의 NotFound는 authority가 없거나 active가 아닐 때만 만든다 (`framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:758-808`). Lease lifetime을 얻지 못하면 positive cache에 넣지 않을 뿐 source NotFound로 바꾸지 않는다. 공통 Store resolver도 active authority의 expired owner를 명시적 `Unavailable`로 보존한다 (`framework/languages/cpp/framework/src/runtime/locations/store_location_resolvers.hpp:339-363`). | DeliveryDispatch exit 0, 12 s, completion marker 확인 (`doc/plan/c016-worklog/gate-final-cpp-node-samples-summary.md:17-28`). |
| .NET | Session binder는 `GetOrCreate` 결과를 bind한다 (`framework/languages/dotnet/samples/DeliveryDispatch/Server/CourierSession/CourierSessionBinder.cs:15-53`). Reassign은 `SendToActor(courierId, ...)`를 호출한다 (`framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchWorker.cs:183-204`, `framework/languages/dotnet/samples/DeliveryDispatch/Server/Dispatch/DispatchZLinkAdapters.cs:14-33`). | Resolver는 raw authority가 있으나 live owner/route admission이 없으면 `KnownUnavailable`을 반환한다 (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkStoreLocationResolvers.cs:223-282,371-410`). Actor client는 이를 public `Unavailable`, 진짜 missing만 `NotFound`로 바꾼다 (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorClient.cs:184-207`). | DeliveryDispatch pass, 18.061 s (`doc/plan/c016-worklog/gate-final-dotnet-samples-2-summary.md:7-16`). |
| Java | Client가 두 courier bind를 순서대로 완료한다 (`framework/languages/java/samples/java/DeliveryDispatch/Client/src/main/java/systems/zlink/samples/deliverydispatch/client/DeliveryDispatchClientScenario.java:21-47`). Session은 `getOrCreate → bind → relay`를 수행한다 (`framework/languages/java/samples/java/DeliveryDispatch/Server/CourierSession/src/main/java/systems/zlink/samples/deliverydispatch/server/couriersession/sessions/CourierSession.java:75-109`). Reassign은 ActorId direct send다 (`framework/languages/java/samples/java/DeliveryDispatch/Server/Dispatch/src/main/java/systems/zlink/samples/deliverydispatch/server/dispatch/DispatchWorker.java:65-113`). | Active authority의 owner lease가 없으면 resolver가 `UNAVAILABLE`로 실패한다 (`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkStoreLocationResolvers.java:282-369`). Actor client는 resolver가 정상적으로 `null`을 반환한 Missing만 `NOT_FOUND`로 바꾼다 (`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorClientRuntime.java:205-217`). | DeliveryDispatch exit 0, 37 s, completion marker 확인 (`doc/plan/c016-worklog/gate-final-java-samples-summary.md:5-15`). |

이전 gate에서 C++/.NET/Java DeliveryDispatch는 모두 같은 reassignment 기능 marker를 내고
통과했다. Green 결과만으로 race 부재를 증명할 수는 없지만, result-kind 계약과 구현을 함께
대조하면 Node만 active-owner-unavailable을 local NotFound로 축약한다.

## 분류, 최소 수정 위치와 회귀 조건

**분류: B — 기존 결함.** Spec gap(D)이 아니다. `08-routing` §2.2가 결과 종류를 이미 닫아
두었다. 새 공개 계약 적응(A)도 아니고, timeout/retry/cache 우회(C)도 아니다.

승인 뒤의 최소 수정 위치는 Node runtime의
`framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts:388-455`다.
`resolveDirectActorRoute`가 다음 경계를 보존해야 한다.

- authority record가 실제로 없을 때만 Missing(`undefined`)을 반환한다.
- authority가 있으나 active/Ready가 아니거나 current owner lease가 usable하지 않으면
  `ZLinkFrameworkErrorKind.Unavailable`을 반환하거나 던진다. 같은 파일의 Spot resolver가 이미
  쓰는 방식
  (`framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts:879-909`)과 Java
  방식이 최소 선례다.
- Store read 실패는 StoreFailure 경로로 그대로 전파한다.
- ReadyRoute만 `directActorRoutes`에 넣는다.

이렇게 하면
`framework/languages/node/packages/framework/src/runtime/actors/actor-client.ts:312-319`의
`undefined → ActorRouteNotFound`는 진짜 Missing에만
적용된다. 닫힌 discriminated result를 도입한다면 actor client도 함께 바뀌지만, 현재 결함에 대한
가장 작은 동작 수정은 resolver가 existing-but-unavailable을 명시적 `Unavailable`로 내보내는
것이다. Sample, deadline, retry 횟수, Core 또는 binding은 수정 대상이 아니다.

다음 회귀가 수정 사실을 증명해야 한다.

1. `test/contract/object-routing.test.js`: active Actor authority와 일치하는 fence를 두고
   `remainingOwnerTokenLeaseMs()`만 0을 반환하게 한다. Direct Actor resolve는 public
   `Unavailable`로 실패하고 route를 cache하지 않아야 한다. 기존 Spot owner-loss test
   (`framework/languages/node/test/contract/object-routing.test.js:185-223`)의 Actor 대칭 테스트다.
2. `framework/languages/node/test/contract/actor-client.test.js`: 위 resolver를 Actor client에 연결한다. `sendToActor`는
   `Unavailable`로 끝나고 native/route transport submit 횟수는 0이어야 한다. 별도 missing case는
   계속 `NotFound`여야 한다.
3. Public process regression: bind 완료 뒤 `courier-b` owner lease를 deterministic test provider로
   unavailable하게 만든 다음 direct send 결과가 `Unavailable`이고 `courier-node-1` handler가
   0회임을 확인한다. 정상 lease에서는 기존 DeliveryDispatch reassignment marker가 그대로
   통과해야 한다. TTL 확대나 sleep으로 race를 숨기면 안 된다.

Owner lease가 usable하지 않게 된 선행 원인을 별도로 고칠 필요가 있다면, 첫 회귀에서 renew
event와 Store 결과를 보존해 원인을 다시 분류해야 한다. 이번 증거만으로 heartbeat 구현을 바꾸는
것은 진단 범위를 넘는다.

## Stage 1 종료 상태

- 소유 계층: source Framework routing/location resolver와 Location runtime owner lease.
- Spec 조항: `08-routing` §2.1–2.2, §3.1–3.2; `session-actor-binding` §2, §4–5;
  `location-runtime` §3.1, §5, §7.3.
- 교차언어: .NET/Java는 owner-unavailable을 명시적으로 `Unavailable`로 보존하고, C++도 lease
  실패만으로 source NotFound를 만들지 않는다. 세 DeliveryDispatch gate는 green이다.
- 변경 분류: **B — 기존 Node Framework 결함**.

Runtime, sample, Core, binding, spec 문서는 수정하지 않았다. 재현용 임시 trace sink도 제거했다.
Stage 1에서 멈춘다.
