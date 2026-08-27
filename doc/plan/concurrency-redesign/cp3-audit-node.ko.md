# CP3 Node 감사 — `await` 경계 상태 소유권

## 1. 결론

**판정: NOT CLEAN.** 테스트를 제외한 Node Framework 구현을 전수 정적 감사한 결과,
`await` 전에 읽은 mutable 상태를 경계 뒤에서 현재성 확인 없이 사용하는 실질 source는
**68곳**이었다. 이 중 **63곳은 정당화**할 수 있고, **5곳은 결함 의심**이다.

- 결함 의심: **[H] 2건, [M] 3건, [C]/[L] 0건**
- L2의 STOP 9건: **9건 모두 변화 없음**, 재분류 필요 0건
- 동기/return-before 호환 경계: **6개 묶음**. blocking bridge는 0개이다.
- POSDDD ②: 정적 우선순위 상위 10개를 §6에 기록했다. 프로파일링 전이므로 성능 결함으로
  확정하지 않는다.

사용자가 지정한 `framework/languages/nodejs` 경로는 현재 checkout에 없다. L2 조사와 정확히
같은 실제 경로인 `framework/languages/node/packages/framework/src`의 TypeScript 373파일을
대상으로 삼았고, `test`/`tests`와 생성 산출물은 제외했다.

## 2. 기준과 측정 방법

판정 기준은 공통 명세의 “mutable authorization을 `await` 뒤에서 다시 확인하지 않고 사용하는
것” 금지와 Node의 동기 JS turn 원자성이다
(`framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:54`,
`:245`, `:296`). 방법은 dotnet CP3 감사와 같되 `lock` 대신 `await` 경계를 source 위치로
계수했다.

1. `rg --files .../src -g '*.ts'`로 373파일을 고정하고 `await` 토큰 2,397개를 확인했다.
2. TypeScript AST로 모든 async 함수/메서드에서 첫 `await` 전 `this.*` 읽기와 이후 field,
   `Map`/`Set`/배열 변경이 함께 있는 source를 추출했다. 경로 비민감 seed는 **94곳**이었다.
3. 각 seed의 분기와 호출자를 수동 추적했다. 서로 배타적인 분기, immutable 옵션 읽기,
   경계 뒤 exact-entry/current-generation 재조회가 있는 26곳을 제외하여 **68곳**을 남겼다.
4. 남은 68곳은 다음 질문으로 판정했다. (a) 경계 전에 exact token/reservation/operation identity를
   설치했는가, (b) drain/lifecycle/stage가 다음 turn을 직렬 소유하는가, (c) 아니라면 mutable
   authorization을 다시 읽는가. (a)/(b)는 정당화, 어느 것도 아니면 결함 의심이다.

이 수치는 `await` 문 개수가 아니라 **상태를 읽고 경계를 넘는 source 함수 위치**의 개수다.
같은 함수의 여러 `await`와 여러 field write는 한 번만 센다. 짧은 코드 인용은 식별에 필요한
표현만 적었다.

## 3. `await` 경계 스냅샷 전수 판정

### 3.1 정당화 63곳

아래 표의 source 수 합계가 63이다. 표 안의 함수 목록은 해당 분류의 전 목록이며, 같은 함수는
한 행에만 포함했다.

| 분류 | source 수 | 파일:라인·함수 | 근거 |
|---|---:|---|---|
| 실행·drain primitive | 14 | `runtime/execution/serial-scheduler.ts:265 drain`, `execution/state-lane.ts:95 drain`, `execution/index.ts:267 enter`, `:309 waitForQuiescence`, `backend/mesh-dispatch-pump.ts:171 drainInfrastructure`, `:187 drainDomain`, `channels/channel-receive-loops.ts:228/453/653 run`, `foundation/event-loop-resources.ts:107 drainApplication`, `spots/spot-actor-join-dispatch.ts:269 drain`, `spot-actor-packet-drain.ts:66 drain`, `spot-serial-executor.ts:252 wrapped`, `:299 callback` | queue head·running flag·permit는 primitive가 소유하는 작업 순서다. 다음 작업은 현재 Promise terminal 뒤에만 진입한다. |
| 단일 poll/route turn 소유 | 6 | `backend/node/node-raw-mesh-backend.ts:1506 poll`, `foundation/raw-service-mesh-runtime.ts:985 drainMonitorEvents`, `spots/spot-routed-frame-dispatch.ts:151 drain`, `spot-subscription-dispatch.ts:78 drain`, `streams/stream-session-runtime.ts:984 runReceiveLoop`, `:633 runLivenessCheck` | poll timer 또는 drain owner가 하나이고, 처리할 batch/received를 현재 turn이 소유한다. 단, routed frame의 재진입 branch는 §3.2의 세대 역전 가능성을 여는 근거이기도 하다. |
| lifecycle terminal 소유 | 12 | `actors/index.ts:201 destroy`, `:632 destroyActor`, `host/index.ts:1255 startCore`, `:1423 stopCore`, `:1489 recovery callback`, `host/route-mesh-runtime.ts:361 prepareHostRetire`, `:408 relocateHost`, `:490 performHostDrain`, `locations/runtime.ts:298 start`, `:367 cleanupOwner`, `host/stateful-authority-route-runtime.ts:126 start`, `:142 stop` | host/start-stop owner가 중복 lifecycle 진입을 막고 terminal까지 소유한다. `locations/runtime.ts:445`는 경계 뒤 owner token을 다시 검사하므로 이 계수에서 제외했다. |
| exact operation·Promise identity | 9 | `foundation/service-relocation-object-owner.ts:40 commitSource`, `:49 abortSource`, `host/instance-activation-authority.ts:86 reserve`, `:271 commit`, `:433 abort`, `host/relocation-direct-transfer.ts:224 acquire`, `streams/actor-session-lifecycle-coordinator.ts:4 run`, `spots/spot-node-runtime-manager.ts:954 ensureEntryActivation`, `host/remote-actor-packet-target-store.ts:44 updateFromWire` | 경계 전 설치한 reservation, Promise tail, entry identity가 작업의 권한이다. cleanup은 같은 identity일 때만 제거한다. |
| relocation stage·terminal 프로토콜 | 12 | `actors/index.ts:424 prepareRelocationActor`, `:524 removeRelocationActor`, `:543 completeCoreRelocationSource`, `host/actor-transfer-runtime.ts:344 commitAndDeliverDeferredJoinAccepted`, `:862 notifyCoreSourceLeave`, `host/service-relocation-host-runtime.ts:488 dispose`, `:1375 relocateSpotAggregate`, `:1956 runCoordinator`, `:2413 handlePrepareControl`, `:3052 sendReplyRelay`, `host/remote-bound-session-relay.ts:248 receive...Seal`, `:370 receive...Route` | stage/relocation identity를 첫 비동기 작업 전에 등록하고 동일 요청은 기존 Promise에 합류한다. 예: seal은 `activeServiceWireRelocations.set(key, state)`를 한 turn에서 수행한다(`remote-bound-session-relay.ts:275-317`). |
| 위치·권한 저장소 프로토콜 | 8 | `channels/client-server-location-runtime.ts:107 reclaimOwnerRows`, `:136 publishServers`, `:406 drainAndRemoveServers`, `channels/fanout-location-runtime.ts:113 reclaimOwnerRows`, `:143 publishLocalPublishers`, `:269 openConnection`, `:387 removeLocalPublishers`, `locations/actor-location-claims.ts:77 claim` | store generation/CAS 결과 또는 한 publish batch가 권한이다. 반환 뒤 쓰기는 그 결과의 terminal 반영이다. 아래의 in-memory map reclaim 두 건은 이 분류에서 제외했다. |
| Spot/stream의 serial owner·소유권 이전 | 2 | `spots/spot-node-runtime-manager.ts:817 dispose`, `streams/stream-session-runtime.ts:953 dispose` | await 전에 소유 컬렉션을 local로 옮기거나 종료 상태를 선점하고, 새 작업 admission을 닫은 뒤 terminal 정리를 한다. |

경계 뒤 exact-current 검사가 있어 계수에서 제외한 대표 예는
`managed-stream.ts:303-308`의 `if (this.nativeActorBindings.get(actorId) === binding)`과
`service-stateful-registry.ts:559-573`의 Promise-tail 동일성 검사다. 이들은 단순히 “단일
스레드라서 안전”한 것이 아니라 명시적 identity fence가 있다.

### 3.2 결함 의심 5곳

| 심각도 | 위치 | 짧은 근거 | 의심되는 실패 |
|---|---|---|---|
| **[H]** | `runtime/streams/managed-stream.ts:173-225` `bindActor` | `transportClosed`를 L183에서 한 번 읽고 L195-210을 await한 뒤 `nativeActorBindings.set(...)` | await 중 `markTransportClosed()`가 L350-352에서 실행돼도 닫힌 transport에 native binding을 다시 설치한다. `sendBoundActor`(L315-340)는 closed fence 없이 이 binding을 사용한다. 경계 뒤 closed 상태와 exact route를 재검증해야 한다. |
| **[H]** | `runtime/host/remote-bound-session-relay.ts:106-128` `receiveRoutedBoundSession` | `currentGeneration`을 L116에서 읽고 target update/rebind를 await한 뒤 L126에서 세대를 쓴다. | 더 새 세대의 turn이 먼저 끝난 뒤 오래된 turn이 map을 낮출 수 있다. `spot-routed-frame-dispatch.ts:151-160`은 drain 중 받은 frame을 별도 await dispatch하므로 실제 겹침 경로가 있다. actor별 직렬 소유 또는 경계 뒤 generation fence가 없다. |
| **[M]** | `runtime/host/actor-packet-relay.ts:622-755` `relayRemoteActorPacket` | actor/session route와 target을 L629-646에서 얻고 원격 요청을 await한 뒤 `rememberActorTarget`/`clear`한다. | 왕복 중 actor tenure 또는 target이 바뀌어도 늦은 reply가 새 cache를 덮거나 지운다. `remote-actor-packet-target-store.ts:165-188`은 tenure key를 만들지만 현재 actor/route와 같은지 검증하지 않고 저장한다. |
| **[M]** | `runtime/locations/actor-location-claims.ts:281-320` `reclaimOwnerRows` | `[...this.actors]`의 `tracked`를 L287에서 잡고 store를 await한 뒤 `delete` 또는 `tracked.row = ...` | recovery 중 같은 key가 release/reclaim돼 교체되면 오래된 작업이 새 entry를 삭제하거나 분리된 객체만 갱신한다. 쓰기 전에 `this.actors.get(canonical) === tracked` 검사가 없다. |
| **[M]** | `runtime/locations/spot-location-claims.ts:232-310` `reclaimOwnerRows` | `[...this.spots]`의 `tracked`를 L238에서 잡고 resolve/CAS를 await한 뒤 L249/L282 delete 또는 L254/L267/L306 write | Actor claim과 같은 stale-entry 문제다. 특히 legacy와 authority 양쪽 모두 exact-entry 재검증 없이 terminal 반영한다. |

다섯 건은 정적 호출 경로로 가능한 interleaving을 확인한 **결함 의심**이며 재현·수정은 이
읽기 전용 작업 범위가 아니다. 특히 reclaim 두 건은 host recovery가 owner token을 마지막에
검증한다(`runtime/host/index.ts:1502-1520`)는 사실만으로 개별 claim map entry의 동일성을
보장하지 않는다.

## 4. STOP 9건 현재 상태

`l2-survey-node.ko.md:183-191`의 순서 22~30이 진행표의 “STOP 9=executor 순서 제외군”에
대응한다. 순서 31~34의 다섯 묶음은 별도 “lane 불요 5”이며 STOP 9에 포함하지 않았다.

| # | L2 묶음 | 현재 근거 | 판정 |
|---:|---|---|---|
| 22 | Spot activation/lifecycle/Entry activation | `spots/spot-activation-state.ts:214`, `spot-activation.ts:161`, `spot-entry-activation.ts:115`; activation operation과 lifecycle terminal이 순서를 소유한다. | 변화 없음 |
| 23 | actor manager + transferred rollback | `actors/index.ts:153`, `:424`, `:524`; relocation stage가 sync turn에서 선점되고 같은 target operation이 완료한다. | 변화 없음 |
| 24 | actor handoff coordinator | `actors/actor-handoff.ts:231`; actor별 operation tail/terminal이 작업 순서를 소유한다. | 변화 없음 |
| 25 | instance activation authority + user-Spot creation coordinator | `host/instance-activation-authority.ts:73`, `user-spot-creation-coordinator.ts:84`; reservation/CAS identity가 권한이다. | 변화 없음 |
| 26 | location runtime + owner + authority-route runtime | `locations/runtime.ts:135`, `host/location-runtime-owner.ts:34`, `stateful-authority-route-runtime.ts:119`; owner lifecycle와 lease token이 순서를 소유한다. claim-map reclaim 의심은 이 STOP owner 자체가 아니라 하위 map terminal 반영 문제다. | 변화 없음 |
| 27 | Spot-node runtime manager + routed actor admission | `spots/spot-node-runtime-manager.ts:160`, `spot-routed-actor-admission.ts:66`; state-publication Promise와 admission operation이 직렬 소유한다. | 변화 없음 |
| 28 | raw service mesh runtime | `foundation/raw-service-mesh-runtime.ts:985`; monitor batch를 현재 drain이 소유한다. | 변화 없음 |
| 29 | Node raw mesh backend + 중첩 service | `backend/node/node-raw-mesh-backend.ts:1493-1514`; 단일 poll timer/receive turn이 소유한다. | 변화 없음 |
| 30 | actor transfer runtime | `host/actor-transfer-runtime.ts:210`, `:862`; transfer operation과 source-leave terminal이 순서를 소유한다. | 변화 없음 |

따라서 STOP 9를 일반 state lane 대상으로 되돌릴 근거는 찾지 못했다. 다만 §3.2의 다섯
의심 건은 “STOP이 안전하므로 주변 호출도 모두 안전하다”는 확대 해석을 금지한다.

## 5. d.ts·return-before 호환 경계

Node에는 `.Wait()`류 blocking bridge가 없다. 남아 있는 것은 public/generated d.ts의 동기 반환
형태 또는 “Promise가 반환되기 전에 등록이 끝난다”는 순서를 보존하는 브리지다.

| 경계 | 현재 계약과 구현 | 잔존 사유 |
|---|---|---|
| deferred Actor Join 등록 | `runtime/actors/actor-join-deferred-scope.ts:102-111`의 `deferActorJoin(...): void` | handler turn 안에서 intent 등록/용량 실패가 완료돼야 한다. discard의 async 부분만 best effort이며 주석도 “defer() is synchronous”라고 고정한다(L173-181). 발견 8·9 경계다. |
| operation registry | `runtime/foundation/operation-registry.ts:69-135`의 `reserve`, `complete`, `fail`, `cancel`, `isPending`, `close`, `size` | reserve가 반환되기 전에 entry와 timeout을 설치하고 reply/timeout/close 경합을 exact generation으로 결정해야 한다. Promise 전환은 registration-before-completion을 약화한다. |
| service discovery registry | `runtime/foundation/service-discovery-registry.ts:38-130`의 admit/disconnect/remove/select/snapshot | await가 없고, descriptor 선택과 map 교체가 한 JS turn에서 끝난다. 호출자 d.ts를 Promise로 바꿀 소유권 이득이 없다. |
| service topology registry | `runtime/foundation/service-topology-registry.ts:74`, `:115-207`의 admit/disconnect/peer | peer admission 비교와 selection rebuild가 동기 turn 하나다. 발견 8에 따라 sync 계약을 유지한다. |
| in-memory provider Store | `runtime/locations/in-memory-provider-location-store.ts:35-137` | 외부 계약은 Promise지만 구현 내부에는 await가 없다. condition 검사와 mutation이 첫 반환 전에 끝나므로 동기 store의 원자적 turn을 보존한다. Promise-shaped return-before 경계다. |
| mesh completion table | `runtime/backend/mesh-completion-table.ts:48-100` `submit` | `operation()`의 동기 재진입을 L76에서 재검사하고, pending을 Promise 반환 전에 L93에서 설치한다. async completion이 등록을 추월하지 못한다는 계약(L48-52)을 보존한다. |

실제 **d.ts 동기 반환 브리지 묶음은 앞의 네 개**, Promise 형태지만 sync-prefix가 계약인
return-before 경계는 뒤의 두 개다. 어느 경우도 thread-blocking이나 busy wait를 사용하지 않는다.

## 6. POSDDD ② 잔여 후보 상위 10개

POSDDD의 “측정 없이 최적화하지 말되 불필요한 할당·복사를 제거 후보로 기록한다”는 기준으로
정적 빈도, payload 크기, 호출 위치를 함께 본 우선순위다. benchmark/heap profile은 실행하지
않았으므로 순위는 조사 출발점이다.

| 순위 | 위치 | 할당·복사 후보 |
|---:|---|---|
| 1 | `runtime/host/service-relocation-host-runtime.ts:2128`, `:2607`, `:4056-4059`, `:4616` | relocation boundary/record를 `Buffer.concat`·`Buffer.from`으로 반복 복사한다. payload 크기와 fanout이 가장 크다. |
| 2 | `runtime/locations/in-memory-authority-store.ts:163-193`, `:630-697`, `:882-916`, `:1130-1162` | authority payload/digest/terminal envelope의 방어적 `Buffer.from`이 write, snapshot, 비교 양쪽에 겹친다. |
| 3 | `runtime/locations/in-memory-location-store.ts:1388-1412` | page 요청마다 전체 rows materialize→sort→slice를 수행한다. |
| 4 | `runtime/channels/channel-socket-registry.ts:170-218`, `:869` | lifecycle/monitor 경로에서 map values를 배열·`Set`으로 여러 번 복제하고 `Promise.allSettled` 배열을 만든다. |
| 5 | `runtime/spots/spot-node-runtime-manager.ts:455-535` | state publication마다 capabilities를 map/spread/sort하고 `Object.fromEntries`로 descriptor를 재구성한다. |
| 6 | `runtime/spots/index.ts:1947`, `:2016`, `:2138`, `:2783`, `:2876-2877` | packet/relocation dispatch의 wire bytes와 frame 배열 복사가 집중된다. |
| 7 | `runtime/foundation/raw-service-mesh-runtime.ts:1533-1550` | 후보 선택마다 index wrapper 배열, `Set`, splice를 만든다. |
| 8 | `runtime/streams/stream-session-runtime.ts:1377`, `:1404` | head가 자랄 때 `splice(0, head)`로 queue backing array를 압축한다. burst 뒤 큰 복사가 될 수 있다. |
| 9 | `runtime/backend/mesh-completion-table.ts:83`, `:144-151` | submit마다 Promise/abort closure를 만들고 completion의 모든 part를 `Buffer.from`으로 복사한다. |
| 10 | `runtime/diagnostics/topology-runtime-projections.ts:55`, `:182` | 관측 publish마다 target/status DTO 배열을 map으로 다시 만든다. diagnostics level과 observer 유무로 게이트 가능한지 측정 대상이다. |

우선 측정할 값은 relocation/completion별 copied bytes, authority-store operation별 `Buffer`
할당 수, topology publish당 객체 수, queue compaction의 이동 원소 수다. 계약상 ownership을 위해
필요한 방어 복사는 프로파일 결과 없이 제거하면 안 된다.

## 7. 검증 범위와 최종 판정

- 수행: 373개 production TypeScript 파일 AST 스캔, 94개 seed의 수동 control-flow/호출자 추적,
  L2 STOP 9와 호환 경계 재확인
- 미수행: build, unit/E2E, runtime race 재현, benchmark/heap profile
- 변경: 이 문서 1개만 생성. source와 frozen common spec은 변경하지 않음

최종적으로 Node CP3는 **결함 의심 5건 때문에 NOT CLEAN**이다. 63곳의 정당화와 STOP 9 유지
판정은 “Node가 단일 스레드”라는 일반론이 아니라 JS turn의 동기 원자성, exact operation identity,
serial drain/lifecycle owner 중 하나를 source별로 확인한 결과다. 다섯 의심 건은 그 조건이 없고
mutable authorization이 실제 `await` 경계를 건너므로, 후속 작업에서는 각 항목에 exact-current
fence 또는 기존 직렬 owner를 적용한 뒤 관련 focused race test로 확인해야 한다.
