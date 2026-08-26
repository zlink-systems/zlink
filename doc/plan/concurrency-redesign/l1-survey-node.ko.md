# L1 표본 조사 — node (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다. L1 전환 요청의 근거로 보존한다.

## 결론

Node의 대응 상태 소유부는 `ZLinkActorSessionBindingRegistry`이며 판정은 **C2**다. 단일 map 조회가 아니라 binding, ingress seal, active frame, relocation, retained outbound 상태가 교차 전이한다.

### `ZLinkActorSessionBindingRegistry`

[actor-session-binding-registry.ts](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/actor-session-binding-registry.ts:131)

- **lock·동기화 수단**
  - lock/mutex/semaphore: **0개**. `recursive_mutex` 해당 없음.
  - 외부 직렬화 수단: **1개**. [`ZLinkActorSessionLifecycleCoordinator`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/actor-session-lifecycle-coordinator.ts:1)가 actor ID별 Promise tail을 사용한다.
  - 이 coordinator는 `ZLinkSessionActorCoordinator`와 `ZLinkBoundActorRelaySender`가 공유하지만, registry를 직접 사용하는 `ZLinkBoundSessionService`와 relocation runtime-owner 경로까지 직렬화하지는 않는다([index.ts](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/index.ts:389)).
  - Registry 내부에는 상호배제 수단이 아니라 완료 조율 수단이 있다: seal waiter, active-frame waiter, relocation `ready`/`terminal`, `applyPromise`, outbound `drainPromise`.

- **보호해야 할 상태**
  - `_routes`: `context`, actor, binding token, session identity, authority fence, `sealId`.
  - Route별 active-frame count와 request set.
  - `_sealWaiters`, `_activeFrameWaiters`.
  - `_relocations`: active seal, seal map, outbound array, arrival sequence, count, drain promise.
  - Relocation state: phase, apply fingerprint/promise, accepted producer proof, ready/terminal completion.
  - `_terminalRelocations`.
  - Registry route와 함께 전이하는 session-local actor map은 [`ZLinkSessionLocalActorBindings`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/session-local-actors.ts:5)에 따로 존재한다.

- **여러 컬렉션·필드 동시 접근**
  - Node에는 lock 블록이 없으므로, 한 동기 JS turn에서 둘 이상의 소유 컬렉션·필드를 직접 함께 사용하는 lexical block을 셌다: **22곳**.
  - 주요 근거는 binding과 local index 동시 변경(L167–223), route와 active-frame 상태(L382–454), relocation seal·queue 설치(L513–565), apply phase와 outbound queue 갱신(L708–798), relocation 정리(L875–925), terminal retention과 queue 제거(L1194–1219)다.
  - 판정: **C2**. 여러 map·field의 불변식과 비동기 행동이 결합하며, C1 concurrent map이나 C3 atomic으로 분리할 수 없다.

- **파급**
  - 구체 타입 외부 사용: **4파일/58지점**.
    - `session-actor-coordinator.ts`: 16
    - `bound-session-service.ts`: 10
    - `bound-actor-relay-sender.ts`: 15
    - `streams/index.ts`: 17
  - `async` 함수 또는 Promise를 반환하는 callback: **33/58, 약 57%**.
  - 동기 함수·constructor: **25/58, 약 43%**.
  - 인터페이스로 지워진 runtime-owner 소비자는 이 수에 포함하지 않았다.

- **재진입 의심**
  - Lane 전환 시 같은 public 표면을 다시 부르는 호출: **19곳**. Private waiter에서 public 상태 표면을 부르는 3곳까지 포함하면 **22곳**이다.
  - 대표 지점:
    - `replaceAndReleaseSeal()` → `replace()`/`abortSeal()` ([L225](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/actor-session-binding-registry.ts:225))
    - `unbindActor()`/`cleanup()` → `unbind()` (L296–307)
    - `acceptWhenReady()` → `requireRoute()`/`requireCurrentToken()`/`accept()` (L359–378)
    - accepted-frame 메서드 상호 호출(L382–463)
    - `sealAndWait()`/`sealRelocation()` → `seal()` (L503–546)
    - waiter private 메서드 → public 조회 표면(L952–1010)
  - 실제 `await` 교차 위험:
    - route를 읽고 authority/native bind를 기다린 뒤 binding을 쓴다: [`session-actor-coordinator.ts:102`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/session-actor-coordinator.ts:102)–160.
    - route를 읽고 authority 조회 후 캡처한 actor/route를 갱신한다: 같은 파일 L329–352.
    - relocation state와 queue를 읽고 `commitOwnerTransition()` 후 같은 참조를 갱신한다: registry L716–798.
    - outbound 첫 entry를 잡고 `deliver()` 후 queue를 갱신한다: registry L1074–1102.
    - route를 읽고 transport disconnect 후 exact route를 제거한다: [`bound-session-service.ts:290`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/streams/bound-session-service.ts:290)–300.
  - 일부 경로는 token·identity 재검사로 stale write를 막지만, 메커니즘상 다른 turn의 개입 자체는 허용한다.

- **lane 밖으로 나가는 snapshot·참조**
  - `find()`는 actor 객체, `route()`와 `requireRoute()`는 mutable route 객체를 직접 반환한다(L254–259, L312–322).
  - `relocationSnapshot()`은 실제 relocation state 객체를 readonly interface로 반환한다(L569–571).
  - `capturePendingReplyClaim()`은 wrapper만 freeze하고 mutable `context` 참조는 그대로 내보낸다(L262–272).
  - accepted-frame admission 객체는 `activeFrames`와 route를 closure로 캡처한다(L382–454).
  - JavaScript의 강한 참조는 map에서 제거된 route/state도 GC되지 않게 유지한다. 따라서 `await` 뒤의 캡처 참조가 현재 map에서 분리된 상태를 계속 갱신할 수 있다.

- **장기 작업**
  - 논리 시작점 **5개**:
    - relocation terminal promise 생성·후속 seal 대기(L527–559)
    - active-frame drain 대기와 timeout(L546, L1009–1057)
    - seal-release waiter timeout(L952–1006)
    - owner transition apply continuation(L757–798)
    - retained outbound drain loop(L1074–1102)
  - `setTimeout`과 Promise continuation은 `AsyncLocalStorage` 문맥을 이어받으므로, lane 안에서 시작하면 lane 소유 표시가 장기 작업으로 전파될 수 있다.

- **POSDDD 관찰**
  - 할당: frame마다 admission closure·객체(L394–454), waiter마다 Promise·closure·timer(L960–1057), relocation마다 terminal/state 객체(L545–560)가 만들어진다.
  - 복사·이동: cleanup의 `[...routes.values()]` snapshot(L304–309), relocation state의 object spread(L554), outbound의 `splice()`와 `shift()`가 배열 원소 이동을 반복한다(L699–703, L1078–1095).
  - 경합: 현재 OS lock 경합은 없다. 전환 후 클래스 전체에 lane 하나를 두고 external await나 outbound delivery까지 turn 안에 유지하면 서로 다른 actor도 head-of-line blocking을 겪을 수 있다. 측정 없이 lane 경계를 쪼갤 근거는 없으므로 구현 단계에서 장기 작업의 문맥 분리와 상태 재진입 방식부터 확정해야 한다.

- **예상 난이도**
  - .NET 표본 대비 **약 1.3배**로 예상한다.
  - 파일 수와 호출 수는 표본보다 작고 Node용 [`ZLinkStateLane`](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/execution/state-lane.ts:12)도 이미 존재한다.
  - 반면 동기 호출 25곳의 Promise 전환, 22곳의 재진입 후보, mutable 참조 escape, coordinator·service·runtime-owner로 나뉜 상태 접근 때문에 단순 wrapper 적용으로 끝나지 않는다.

- **걸린 시간:** 약 **18분**.

파일은 수정하지 않았다. `git` 명령, build, test도 실행하지 않았다. 따라서 사용자가 지정한 branch는 별도로 확인하지 않았다.
