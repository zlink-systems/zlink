# Stage 2 Node direct Actor resolver 결과

## 결과

Node direct Actor resolver가 이제 `ready`, `missing`, `owner_unavailable`을 판별형 결과로
반환한다. Active authority가 있지만 owner lease의 남은 시간이 0 이하이면 Actor client는
`ActorRouteUnavailable`을 public `Unavailable`로 바꾼다. Authority snapshot이 없을 때는 기존처럼
`ActorRouteNotFound`와 public `NotFound`를 반환한다.

Owner lease를 사용할 수 없다고 판정한 시점에는 기존 relocation debug 경로에
`actor_route.owner_lease_observed`를 기록한다. 이 event에는 `actorId`,
`authorityGeneration`, `remainingLeaseMs`, `decision=owner_unavailable`이 들어간다. Actor ID와
generation을 일반 structured log에 기록하지 않도록 `ZLINK_DEBUG_FRAMEWORK_RELOCATION=1`일 때만
출력한다.

## Diff

- `packages/framework/src/runtime/locations/resolvers.ts`
  - `ZLinkDirectActorRouteResolution`을 추가했다.
  - expired owner lease를 `owner_unavailable`로 보존하고 관측 event를 발생시킨다.
  - Ready route만 positive cache에 넣는 기존 규칙은 유지했다.
- `packages/framework/src/runtime/actors/actor-client.ts`
  - `missing`은 `ActorRouteNotFound`, `owner_unavailable`은 `ActorRouteUnavailable`로 바꾼다.
- `packages/framework/src/runtime/framework-errors-internal.ts`
  - `ActorRouteUnavailable` internal kind와 public `Unavailable` mapping을 추가했다.
  - internal failure code 42를 기존 Unavailable terminal과 같은 방식으로 매핑했다.
- `packages/framework/src/runtime/diagnostics/index.ts`
  - 제한된 relocation debug 경로에 owner-lease observation을 출력하는 emitter를 추가했다.
- `packages/framework/src/runtime/host/index.ts`
  - binding authority fence adapter가 판별형 resolver 결과에서 `ready.route`만 사용하도록 맞췄다.
  - unavailable/missing을 fence 없음으로 처리하는 기존 동작은 바꾸지 않았다.
- `test/contract/actor-client.test.js`, `monitoring-runtime.test.js`
  - expired lease의 `Unavailable`, missing snapshot의 `NotFound`, 실제 resolver 분기의 관측 payload를
    검증한다.
- `test/contract/object-routing.test.js`, `deferred-actor-join.test.js`, `stream-runtime.test.js`
  - 기존 Ready/Missing fixture를 판별형 결과에 맞췄다.

변경량은 10개 파일, 269줄 추가, 67줄 삭제다. 이 문서는 변경량에 포함하지 않았다.
`core/**`, `bindings/**`, 다른 언어, 보호된 spec 문서, sample, timeout, lease 설정과 retry는
수정하지 않았다. 다른 Node 작업자가 수정 중인
`backend/node/node-raw-mesh-backend.ts`, `foundation/service-stateful-runtime.ts`와 해당 deliver
consumer는 수정하지 않았다.

## 필수 판정 네 줄

- 소유 계층: Global Actor ID를 current owner route로 바꾸고 terminal 종류를 정하는 Node source Framework의 routing/location resolver와 Actor client mapper가 소유한다. Core·binding의 연결 선택이나 재연결 상태를 추가하지 않았다.
- Spec 조항: `08-routing` §2.1은 source runtime의 direct resolution을, §2.2는 `ReadyRoute / Missing / Unavailable / StoreFailure` 결과와 Ready-only positive cache를 규정한다. `06-observability` §9에 따라 high-cardinality Actor ID는 일반 structured log가 아니라 제한된 debug 경로에서만 기록했다.
- 교차언어 대조: .NET은 active authority에 usable owner가 없으면 `KnownUnavailable`, Java는 owner lease admission 실패를 `UNAVAILABLE`로 유지한다. C++도 lease 실패에서 source `NotFound`를 만들지 않는다. Node만 `undefined`로 축약하던 내부 반환 형태가 달라 Node resolver와 그 내부 호출부만 변경했다.
- 변경 분류: **B — 기존 결함 수정**. 이미 닫힌 resolver 결과 계약에서 owner lease 실패를 Missing으로 잘못 축약한 구현을 수정했다.

## 검증 결과

모든 명령은 `framework/languages/node`에서 `TMPDIR=/dev/shm/zlink-tmp-node`,
`unset ZLINK_LIBRARY_PATH`, `flock -w7200 /tmp/zlink-node-gate.lock` 조건으로 실행했다.

- `npm run build`: 통과.
- touched test 5개 동시 실행: 219/219 통과.
  - `actor-client.test.js`
  - `monitoring-runtime.test.js`
  - `object-routing.test.js`
  - `deferred-actor-join.test.js`
  - `stream-runtime.test.js`
- `actor-client.test.js` 최종 재실행: 14/14 통과.
- `framework-error-wire.test.js`: 4/4 통과.
- `npm run typecheck`: 통과.
- `npm run lint`: 통과.
- `bash samples/run_samples.sh DeliveryDispatch.Ts`: 3/3 통과. 세 번 모두
  `deliverydispatch-reassignment=completed`, `deliverydispatch-server-evidence=completed`,
  `deliverydispatch=completed`, `deliverydispatch-placement=completed` marker를 확인했다.
- `git diff --check`: 통과.

## BLOCKERS

없음.
