# CP3 Node 결함 의심 5건 검증 판정

## 1. 판정 기준과 범위

이 문서는 `cp3-audit-node.ko.md` §3.2의 결함 의심 5건을 현재 소스로 다시 추적한
읽기 전용 정적 검증 결과다. 판정 기준은 다음과 같다.

- `await` 전에 읽은 mutable authorization을 경계 뒤에서 쓰려면 같은 작업을 끝까지 소유하는
  직렬 owner나 exact identity fence가 있어야 한다
  (`06-state-ownership-and-lanes.ko.md:54-73`, `:100-120`, `:255-267`).
- Node의 동기 메서드는 한 JavaScript turn 안에서 원자적이다. 따라서 최소 수정은 동기 표면을
  Promise로 바꾸지 않고, 기존 비동기 경로에 generation·entry identity fence를 둔다
  (`rules.ko.md:213-216`, `06-state-ownership-and-lanes.ko.md:296-299`).
- build·test·재현과 git 명령은 실행하지 않았다. 아래의 `[실증]`은 실행 결과가 아니라, 서로
  독립적으로 진입할 수 있는 호출 경로와 상태 변경 지점을 소스에서 끝까지 확인했다는 뜻이다.

다섯 건 모두 상위 호출자에 전체 경계를 소유하는 공통 직렬 owner나 exact identity fence가
없었다. 최종 판정은 **[실증] 5건, [반박] 0건, [미결] 0건**이다.

## 2. [H] `ZLinkManagedStream.bindActor`의 closed fence 누락

**판정: [실증].** `bindActor`를 직렬화하는 actor별 operation tail은 transport 종료를 소유하지
않는다. 종료 표시가 native bind의 `await` 중 독립 callback에서 바뀔 수 있고, 완료 경로는 이를
다시 확인하지 않는다.

### 도달 가능한 호출 경로

1. application은 `DefaultZLinkSessionActors.bind`
   (`runtime/streams/session-context.ts:454-456`)에서
   `ZLinkStreamBindingRuntime.bind`(`runtime/streams/index.ts:442-447`)으로 들어간다.
2. `ZLinkSessionActorCoordinator.bind`는 actor별 lifecycle tail을 잡고
   `replaceBinding`을 기다린다(`runtime/streams/session-actor-coordinator.ts:54-63`). 그 안의
   `bindNativeActor`가 `context.stream.bindActor`를 호출한다(`:440-454`).
3. `ZLinkManagedStream.bindActor`는 `transportClosed`를 한 번 확인한 뒤
   (`runtime/streams/managed-stream.ts:183-188`), route 조회와 native bind 완료를 기다린다
   (`:189-210`). 완료 후 closed 상태를 다시 읽지 않고 binding snapshot을 map에 넣는다
   (`:211-225`).
4. 같은 stream의 binding-replacement callback은 session serial turn에서 timer를 등록하지만,
   timer callback 자체는 serial queue에 다시 들어가지 않고 `this.close()`를 직접 호출한다
   (`runtime/streams/stream-session-runtime.ts:359-381`). 따라서 3번의 `await`가 열린 같은
   event-loop 구간에서 별도 JavaScript turn의 timer callback으로 진입할 수 있다.
5. `ZLinkStreamSessionRuntime.close`는 첫 `await` 전에 `markTransportClosed`를 호출한다
   (`runtime/streams/stream-session-runtime.ts:432-435`). 실제 표시는 동기 메서드
   `ZLinkManagedStream.markTransportClosed`가 바꾼다
   (`runtime/streams/managed-stream.ts:349-352`). 이 메서드는 2번의 actor별 tail에 속하지 않는다.
6. 3번이 재개되면 닫힌 transport의 `nativeActorBindings`에 binding이 다시 생긴다. 이후
   `sendBoundActor`는 closed 상태를 확인하지 않고 이 map entry로 native send를 수행한다
   (`runtime/streams/managed-stream.ts:315-340`).

따라서 actor별 lifecycle tail(`actor-session-lifecycle-coordinator.ts:4-19`)은 같은 actor의
bind/rebind 순서만 보장하며, stream timer가 바꾸는 `transportClosed`와 map 설치 사이의
불변식은 보호하지 않는다.

### 최소 수정 제안

- `runtime/streams/managed-stream.ts:195`의 route 확인 완료 뒤와 `:221`의 map 설치 직전에
  `transportClosed`를 다시 확인한다.
- native bind가 이미 성공한 뒤 두 번째 fence가 닫힘을 발견하면, 방금 얻은
  `binding.bindingGeneration`과 actor identity로 exact idempotent unbind를 제출하고 map에는 넣지
  않은 뒤 `RouteNotConnected`로 끝낸다. `sendBoundActor`의 map 사용 전(`:320`)에도 closed fence를
  둬 이미 시작된 정리 창에서 stale entry를 쓰지 못하게 한다.
- 공개·동기 signature를 바꿀 필요는 없다. 기존 `bindActor`/`sendBoundActor` 비동기 경로 안의
  fence만 보강한다.

### 캠페인 회귀 여부

**판정 불가 — 중앙에서 git 이분 필요.** 이 파일에는 `state-lane` import나 lane `run` 호출이
없다. L2 조사에는 managed stream이 전환 후보로 기록돼 있지만
(`l2-survey-node.ko.md:45-46`, `:181`), 현재 구조만으로 base `3cbfbde4f9`에도 같은 closed
검사·timer 호출 구조가 있었는지 확인할 수 없다. 정적 형태는 기존 결함 쪽에 가깝지만 회귀로
확정하지 않는다.

## 3. [H] `receiveRoutedBoundSession`의 ownership generation 역전

**판정: [실증].** routed bound-session receive와 command 44 relocation commit이 같은
`actorOwnershipGenerations` map을 서로 다른 비동기 소유 경로에서 갱신한다. 오래된 receive가
기다리는 동안 command 44가 더 큰 generation을 기록한 뒤, 오래된 receive가 map을 낮출 수 있다.

### 도달 가능한 호출 경로

1. routed-readable backend callback은 `ZLinkSpotActorJoinDispatch.attach`에서 별도 detached 작업으로
   `dispatchFromEvent`를 시작한다
   (`runtime/spots/spot-actor-join-dispatch.ts:222-246`, `:261-266`).
2. `dispatchFromEvent`는 `routeDraining`을 검사하거나 drain owner에 합류하지 않고 곧바로
   `dispatch`를 기다린다(`runtime/spots/spot-routed-frame-dispatch.ts:203-207`). `dispatch`는
   routed bound-session decoder로 넘긴다(`:217-223`).
3. decoder는 decoded send를 `routedBoundSessionReceiver`에 전달한다
   (`runtime/spots/spot-routed-bound-session-dispatch.ts:53-74`). 이 callback은
   `ZLinkRemoteBoundSessionRelay.receiveRoutedBoundSession`에 연결돼 있다
   (`runtime/spots/spot-actor-join-dispatch.ts:155-180`).
4. receive는 현재 generation을 읽고 오래된 frame만 거른 뒤
   (`runtime/host/remote-bound-session-relay.ts:106-120`), packet target 갱신과 actor rebind를
   기다린다(`:121-124`). generation map은 아직 선점하지 않았다.
5. 그 사이 독립 service-wire control 경로가 command 44를
   `receiveServiceWireSessionRelocationRoute`에 전달할 수 있다
   (`runtime/host/index.ts:741-743`,
   `runtime/host/remote-bound-session-relay.ts:370-437`). commit은 exact relocation state 아래서
   `commitActorRoute`를 끝낸 뒤 target authority generation을 같은 map에 기록한다
   (`runtime/host/remote-bound-session-relay.ts:489-542`, 특히 `:527-540`).
6. 4번의 오래된 receive가 재개되면 command 44가 기록한 더 큰 값과 다시 비교하지 않고
   자신의 작은 값을 map에 쓴다(`runtime/host/remote-bound-session-relay.ts:125-126`). 이어지는
   local send와 fallback submit도 그 오래된 turn에서 진행한다(`:128-142`).

command 44의 relocation identity는 command 44 중복을 직렬화할 뿐(`:394-447`), 일반 routed
receive를 포함하지 않는다. `streamBindingRuntime().rebindActor`의 actor별 tail 역시 rebind 본문만
직렬화하며, 그 앞의 target 갱신과 그 뒤의 generation map 쓰기는 소유하지 않는다. 따라서 상위
fence로 반박할 수 없다.

### 최소 수정 제안

- `runtime/host/remote-bound-session-relay.ts:114-121`에서 generation이 있는 receive를 첫 `await`
  전에 동기 turn으로 admission한다. 현재 값보다 작으면 반환하고, 더 크면 그 값을 map에 먼저
  설치해 이 turn의 generation token으로 캡처한다.
- `:121`과 `:123`의 각 `await` 뒤, rebind 전과 delivery 전마다 map이 캡처한 generation과 같은지
  다시 확인한다. 더 새 generation이 설치됐으면 오래된 turn은 rebind·delivery·fallback을 진행하지
  않는다.
- command 44의 `:537-540`도 현재 값보다 작은 generation으로 map을 낮추지 않는 단조 갱신으로
  맞춘다. 이 방식은 Node 동기 turn의 원자성을 쓰므로 새 Promise 표면이나 별도 lane이 필요 없다.

### 캠페인 회귀 여부

**판정 불가 — 중앙에서 git 이분 필요.** `remote-bound-session-relay.ts`는 L1의 13파일 async 전파
대상에 포함돼 있다(`l2-survey-node.ko.md:121-135`). 따라서 현재 `await` 배치는 lane 전환의 영향을
받았을 가능성이 있다. 그러나 command 44와 기존 rebind가 base에서도 이미 비동기였는지는 현재
소스만으로 확정할 수 없다.

## 4. [M] `relayRemoteActorPacket`의 stale target cache 반영

**판정: [실증].** request relay는 actor별 lifecycle tail에서 network terminal까지 소유하지 않도록
의도적으로 분리돼 있다. 그 왕복 중 command 44가 Session Actor route를 바꿀 수 있으며, 늦은
reply는 요청 시작 때의 tenure가 여전히 현재인지 확인하지 않고 actor-id 전역 cache를 쓰거나
지운다.

### 도달 가능한 호출 경로

1. Session handler가 `DefaultZLinkSessionActor.relay`를 호출하면
   (`runtime/streams/session-context.ts:500-530`), stream binding runtime
   (`runtime/streams/index.ts:658-665`)과 `ZLinkBoundActorRelaySender.relay`
   (`runtime/streams/bound-actor-relay-sender.ts:48-77`)를 거친다.
2. request frame은 actor별 lifecycle tail에서 실제 completion을 시작하지만, completion 자체를
   기다리지 않고 tail을 반납한다
   (`runtime/streams/bound-actor-relay-sender.ts:108-140`, 특히 `:116-138`). 실제 host relay callback은
   `:163-180`에서 기다리며, host가 이를 `ZLinkActorPacketRelay.relayActorPacket`에 연결한다
   (`runtime/host/index.ts:442-445`).
3. `relayActorPacket`은 `relayRemoteActorPacket`을 호출한다
   (`runtime/host/actor-packet-relay.ts:481-500`). 이 함수는 response target, local node, Session route,
   aggregate route와 target을 차례로 읽고(`:628-646`) remote request 왕복을 기다린다
   (`:729-747`).
4. 2번에서 actor별 tail을 이미 반납했으므로, 왕복 중 3절의 command 44 경로가
   `commitActorRoute`를 실행할 수 있다
   (`runtime/host/remote-bound-session-relay.ts:527-540`). 이 호출은
   `ZLinkSessionActorCoordinator.commitActorRoute`의 actor별 tail에서 route를 교체하고
   (`runtime/streams/session-actor-coordinator.ts:313-369`), 재사용한 Session Actor의 ref도 바꾼다
   (`runtime/streams/session-actor-coordinator.ts:122-160`,
   `runtime/streams/session-context.ts:482-497`).
5. 오래된 remote reply가 돌아오면 현재 route를 다시 읽지 않고 target을 cache에 기억하거나 actor
   전체 cache를 지운다(`runtime/host/actor-packet-relay.ts:748-755`).
   `rememberActorTarget`은 actor manager의 현재 state를 actor ID만으로 덮고
   (`runtime/host/remote-actor-packet-target-store.ts:165-170`), 완료 시점의 mutable `actor.ref`로
   tenure key를 새로 만든 뒤 세 cache에 쓴다(`:173-188`, `:192-205`). 따라서 오래된 reply가 새
   tenure의 key로 귀속될 수도 있다. `clear`도 actor ID의 모든 tenure를 지운다(`:64-79`).

초기의 `responseTarget`은 reply를 원래 Session으로 보내기 위한 exact target일 뿐이다. target cache
갱신 권한을 증명하지 않는다. `sessionActorPacketTargetTenureKey`도 저장할 때 계산할 뿐, 요청 시작
시점의 key와 현재 key가 같은지 비교하지 않으므로 fence가 아니다.

### 최소 수정 제안

- `runtime/host/actor-packet-relay.ts:628-646`에서 request가 사용할 Session route와 actor tenure를
  immutable exact identity로 캡처한다. 적어도 actor ID, object generation, node RID,
  binding generation, ownership generation, owner lease generation을 포함한다.
- network `await`가 끝난 `:748` 직후 `streamRuntime.sessionRouteFence(actor.actorId)`와 aggregate
  committed route를 다시 읽어 캡처한 identity와 같을 때만 `rememberActorTarget` 또는 `clear`를
  실행한다. 다르면 reply payload의 target hint는 버리되, 이미 캡처한 response target을 통한 원래
  request terminal 처리는 유지한다.
- store API를 고친다면 `rememberActorTarget`/`clear`에 expected tenure key를 넘기고, 현재
  Session Actor와 key가 같을 때만 actor-id 전역 state와 cache를 바꾸게 한다. 동기 cache 메서드를
  Promise로 바꾸지 않는다.

### 캠페인 회귀 여부

**판정 불가 — 중앙에서 git 이분 필요.** `actor-packet-relay.ts`와 target store는 L1 async 전파
범위였다(`l2-survey-node.ko.md:121-135`). 다만 핵심 stale 창은 remote network request
(`actor-packet-relay.ts:736-747`) 자체이므로 lane 전환 전에 같은 terminal cache 반영이 있었는지는
base 대조 없이는 판단할 수 없다.

## 5. [M] `ZLinkActorLocationClaims.reclaimOwnerRows`의 stale entry 반영

**판정: [실증].** owner recovery는 owner token의 현재성을 한 번 확인한 뒤 시작하지만, 개별 actor
claim map entry를 직렬 소유하지 않는다. recovery가 캡처한 `TrackedActor`가 release와 후속 claim으로
교체돼도, recovery는 exact entry를 확인하지 않고 삭제·takeover·terminal 갱신을 계속한다.

### 도달 가능한 호출 경로

1. fresh owner lease가 설치되면 `ZLinkLocationRuntime.claimFreshOwnerLease`가 renewed handler를
   동기 호출한다(`runtime/locations/runtime.ts:445-485`). host handler는 transport publication 뒤
   `currentLifecycle.reclaimOwnerRows`를 비동기로 시작한다
   (`runtime/host/index.ts:1470-1520`).
2. `ZLinkLocationLifecycle.reclaimOwnerRows`는 authority recovery 다음 actor·Spot reclaim을 직접
   호출한다(`runtime/locations/lifecycle.ts:237-240`). 별도 state lane이나 claim별 tail은 없다.
3. actor reclaim은 map snapshot의 `tracked`를 잡고 store resolve를 기다린다
   (`runtime/locations/actor-location-claims.ts:281-290`).
4. 이때 actor destroy는 native destroy 뒤 `locationLifecycle.releaseActor`에 진입할 수 있다
   (`runtime/actors/index.ts:201-260`, 특히 `:223-249`). lifecycle은 이를 claim 객체에 바로 전달한다
   (`runtime/locations/lifecycle.ts:159-160`). `release`는 store remove 뒤 exact old entry이면 map에서
   지운다(`runtime/locations/actor-location-claims.ts:245-260`).
5. map이 비면 새 actor creation은 `executeActorClaimThenActivate`로 같은 key를 다시 claim할 수 있다
   (`runtime/actors/actor-creation.ts:35-57`, `runtime/locations/lifecycle.ts:69-76`). claim은 store write
   뒤 새 `TrackedActor`를 같은 canonical key에 넣는다
   (`runtime/locations/actor-location-claims.ts:77-123`).
6. 3번의 recovery가 재개되면 `this.actors.get(canonical) === tracked`를 확인하지 않는다. store
   결과에 따라 새 entry를 key만으로 삭제하고 옛 deactivate를 호출하거나(`:291-297`), 분리된 옛
   객체만 갱신한다(`:299-300`, `:313-319`). takeover store write 전에도 identity fence가 없다
   (`:303-306`).

repository provider의 CAS는 일부 stale store write를 거부하지만
(`runtime/locations/location-store-repository.ts:2352-2410`), in-memory store의 explicit Takeover는
현재 row를 허용한다(`runtime/locations/in-memory-location-store.ts:603-637`, `:1444-1457`). 따라서
provider 구현에 따라 우연히 막히는 것은 claim map의 owner fence가 아니다. 또한 host의 owner-token
검사는 reclaim 호출 전의 lease만 검증하며(`runtime/host/index.ts:1508-1516`), 개별 map entry의
동일성을 보장하지 않는다.

### 최소 수정 제안

- `runtime/locations/actor-location-claims.ts:290`의 첫 `await` 뒤부터 모든 terminal 반영 전에
  `this.actors.get(canonical) === tracked`를 확인한다.
- 구체적으로 delete/deactivate 전(`:295`), takeover write 전(`:303`), 성공 결과를 row와 generation에
  쓰기 전(`:313`)에 exact-entry fence를 둔다. fence가 실패하면 그 snapshot 항목은 조용히 건너뛴다.
- delete는 `Map.delete(canonical)`만 호출하지 말고 exact entry가 맞을 때만 수행한다. 동기
  `owns`/`snapshot` 표면은 그대로 둔다.

### 캠페인 회귀 여부

**판정 불가 — 중앙에서 git 이분 필요.** actor claims와 lifecycle은 L2 batch 10 후보였다
(`l2-survey-node.ko.md:89`, `:171`). 현재 대상 파일에는 `state-lane` import나 lane `run` 호출이
없어 직접적인 lane 전환 산물로 보이지 않지만, base의 reclaim 구현과 같은지는 현재 소스만으로
확정할 수 없다.

## 6. [M] `ZLinkSpotLocationClaims.reclaimOwnerRows`의 stale entry 반영

**판정: [실증].** actor claim과 같은 stale-entry 창이 있으며, legacy row와 authority row 모두
exact map entry 확인 없이 terminal 결과를 반영한다. authority store의 CAS가 store overwrite를
막더라도 새 in-memory entry의 삭제와 분리된 객체 갱신은 막지 않는다.

### 도달 가능한 호출 경로

1. owner lease recovery는 5절 1~2번과 같은 경로로 Spot reclaim에 들어간다
   (`runtime/locations/runtime.ts:445-485`, `runtime/host/index.ts:1470-1520`,
   `runtime/locations/lifecycle.ts:237-240`).
2. `ZLinkSpotLocationClaims.reclaimOwnerRows`는 map snapshot의 `tracked`를 잡는다
   (`runtime/locations/spot-location-claims.ts:232-238`). legacy entry는 Spot resolve를 기다리고
   (`:240-244`), authority entry는 authority read를 기다린다(`:277-278`).
3. legacy Spot 종료는 activation cleanup에서 `releaseLocation`으로 들어간다
   (`runtime/spots/spot-activation.ts:920-947`). user Spot 경로는
   `ZLinkSpotLocationClaim.release`(`runtime/spots/index.ts:433-436`,
   `runtime/spots/spot-location-claim.ts:59-64`)를 거쳐 `ZLinkSpotLocationClaims.release`로 간다
   (`runtime/locations/lifecycle.ts:218-224`). legacy release는 map을 먼저 지우고 store remove를
   기다린다(`runtime/locations/spot-location-claims.ts:97-121`). 후속 claim은 store write 뒤 같은
   key에 새 entry를 넣는다(`:41-80`).
4. authority Spot도 application close terminal에서 `releaseInstanceAuthority`로 들어가며
   (`runtime/spots/index.ts:820-881`), host가 이를 `currentLifecycle.releaseSpot`에 연결한다
   (`runtime/host/index.ts:2017-2019`). 동시에 새 Ready activation이나 relocation terminal은
   `trackInstanceSpot`으로 같은 key의 새 authority entry를 동기 설치할 수 있다
   (`runtime/host/index.ts:1626-1639`,
   `runtime/host/service-relocation-host-runtime.ts:4308-4321`,
   `runtime/locations/spot-location-claims.ts:84-95`).
5. 2번의 recovery가 재개되면 exact entry를 확인하지 않는다. legacy branch는 key만으로 삭제하거나
   옛 객체를 갱신한다(`runtime/locations/spot-location-claims.ts:245-273`). authority branch도
   key만으로 삭제하고 옛 deactivate를 호출하거나, CAS 결과를 옛 객체에 쓴다(`:277-310`).

legacy store Takeover는 actor와 같은 explicit takeover 규칙을 쓰므로 provider 구현에 따라 후속
claim을 stale snapshot으로 덮을 수 있다. authority CAS는 store version을 지키지만, 그 CAS와
`this.spots` entry identity는 별개다. 상위 activation close gate와 relocation identity도 각자의
operation을 직렬화할 뿐 owner-recovery loop와 같은 map owner를 공유하지 않는다.

### 최소 수정 제안

- `runtime/locations/spot-location-claims.ts:238`에서 캡처한 `tracked`를 기준으로, 각 `await` 뒤와
  모든 terminal 반영 전에 `this.spots.get(canonical) === tracked`를 확인한다.
- legacy branch의 delete/deactivate(`:249-250`), takeover write(`:257`), row/generation 갱신
  (`:267-273`)과 authority branch의 delete/deactivate(`:282-283`), CAS(`:287`), storeVersion·lease
  갱신(`:306-309`)에 같은 exact-entry fence를 둔다.
- `trackInstanceAuthority`와 `release`의 동기 signature는 유지한다. 필요한 것은 Promise 전환이
  아니라 비동기 reclaim terminal의 identity 조건이다.

### 캠페인 회귀 여부

**판정 불가 — 중앙에서 git 이분 필요.** Spot claims는 actor claims와 함께 L2 batch 10 후보였다
(`l2-survey-node.ko.md:89`, `:171`). 현재 파일에는 lane import나 `run` 호출이 없고 stale 창은
store·authority I/O 자체에 걸쳐 있다. base 대조 없이 기존 결함과 캠페인 회귀를 구분할 수 없다.

## 7. 요약

| # | 원 의심 심각도 | 판정 | 캠페인 회귀 여부 | 최소 수정 제안 |
|---:|---|---|---|---|
| 1 | [H] | **[실증]** closed 뒤 native binding 재설치·사용 가능 | 판정 불가 — 중앙 git 이분 필요 | 있음 — await 뒤 closed·exact binding fence |
| 2 | [H] | **[실증]** command 44가 올린 generation을 오래된 routed receive가 낮출 수 있음 | 판정 불가 — 중앙 git 이분 필요 | 있음 — 첫 await 전 단조 generation admission + 경계 뒤 재검증 |
| 3 | [M] | **[실증]** route 변경 뒤 늦은 remote reply가 새 tenure cache를 덮거나 지울 수 있음 | 판정 불가 — 중앙 git 이분 필요 | 있음 — request 전 exact tenure 캡처 + reply 뒤 current 비교 |
| 4 | [M] | **[실증]** actor reclaim snapshot이 교체 entry에 terminal 반영 가능 | 판정 불가 — 중앙 git 이분 필요 | 있음 — store I/O 전후 exact map-entry fence |
| 5 | [M] | **[실증]** legacy·authority Spot reclaim 모두 교체 entry에 terminal 반영 가능 | 판정 불가 — 중앙 git 이분 필요 | 있음 — 모든 branch의 exact map-entry fence |

정적 결론은 **5건 모두 실제 결함 경로가 존재한다**는 것이다. 다만 base `3cbfbde4f9`와의 source
차이를 git 없이 확인할 수 없으므로, 다섯 건의 기존 결함/캠페인 회귀 구분은 중앙 이분 작업으로
남긴다.

## 8. 중앙 git 이분 판정 (Claude, 2026-08-27)

§7이 5건 모두 "판정 불가 — 중앙 git 이분 필요"로 남긴 캠페인 회귀 여부를 base
`3cbfbde4f9` 대조로 확정했다. 에이전트는 git 사용이 금지돼 있었다.

| # | 파일 | base 대비 diff | 판정 |
|---:|---|---|---|
| 1 | `streams/managed-stream.ts` | **0** | **기존 결함** |
| 2 | `host/remote-bound-session-relay.ts` | 29+/29− | **기존 결함** (아래 근거) |
| 3 | `host/actor-packet-relay.ts` | 34+/32− | **부분 회귀** (아래 근거) |
| 4 | `locations/actor-location-claims.ts` | **0** | **기존 결함** |
| 5 | `locations/spot-location-claims.ts` | **0** | **기존 결함** |

1·4·5는 파일이 base와 **한 글자도 다르지 않다**. 캠페인이 만든 결함일 수 없다.

### #2가 기존 결함인 근거

캠페인은 `updateRemoteActorPacketTarget`를 sync→`await`로 바꿨다. 그러나 §3이 지목한
**세대 역전 불변식** 자체는 창이 넓어지지 않았다.

- 세대 WRITE(`:126`)는 `ownershipGeneration !== undefined`일 때만 실행되고,
  `ownershipGeneration`은 `actorRef?.ownershipGeneration`이다 → **WRITE ⟹ `actorRef !== undefined`**.
- base에서도 `actorRef !== undefined`면 `await ...rebindActor(actorRef)`가 READ와 WRITE 사이에 있었다.

즉 base에도 이미 `READ → await → WRITE` 경로가 정확히 같은 조건에서 존재했다. 캠페인이 추가한
await는 그 앞줄이라 불변식 위반을 새로 만들지 않는다.

### #3이 부분 회귀인 근거 — **캡처 블록의 원자성 상실**

base의 `relayRemoteActorPacket` 캡처 3콜은 **전부 동기**였다.

```
const responseTarget = streamRuntime.captureBoundSessionResponseTarget(actor);   // sync
const storedRoute    = streamRuntime.sessionRouteFence(actor.actorId);           // sync
const aggregateRoute = owner?.committedRoute(actor.actorId);                     // sync
const storedActorRef = aggregateRoute?.actor ?? storedRoute?.actor ?? actor.ref;
```

세 값은 **한 JS turn 안에서 원자적으로** 잡혔고, `storedActorRef`는 서로 정합한 스냅샷이었다.
전환 후 세 콜이 모두 `await`가 되면서 캡처 블록 **내부에 경계 2개가 새로 생겼다** —
`responseTarget`을 잡은 뒤 `storedRoute`를 읽기 전에 route가 바뀔 수 있고, 그 결과
`storedActorRef`가 **서로 다른 시점의 값을 섞은 스냅샷**이 된다.

- **원격 왕복 뒤 stale cache**(감사가 지목한 부분)는 base에도 있었다 → 기존 결함.
- **캡처 블록 자체가 찢어지는 창**은 base에 없었다 → **캠페인 회귀**.

이것은 캠페인이 없애려던 "async 경계 스냅샷" 결함 유형을 캠페인이 새로 만든 사례이고,
발견 9("등록·캡처는 반환 전에 완료한다")의 캡처 측 변형이다. rules §7에 새 유형으로 올릴 후보다.
