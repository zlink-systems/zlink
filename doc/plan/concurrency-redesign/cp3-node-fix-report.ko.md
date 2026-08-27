# CP3 Node 결함 5건 수정 보고

## 범위와 STOP

- STOP: 없음.
- 동기 공개 표면과 d.ts signature는 바꾸지 않았다. Promise/lane 전환도 추가하지 않았다.
- 관측 순서·타임아웃·오류 분류는 유지했다. 새 fence가 막는 것은 이미 새 tenure/closed 상태가
  설치된 뒤의 stale terminal 반영뿐이다.
- `runtime/host/remote-actor-packet-target-store.ts`는 3번의 exact tenure 조건을 동기 cache
  mutation에 전달하기 위해서만 수정했다. `rememberActorTarget`·`clear`는 여전히 동기다.

## 1. [H] managed stream closed fence

- 변경: `runtime/streams/managed-stream.ts:195-256`에서 route 확인 뒤와 native bind 완료 뒤
  `transportClosed`를 재확인했다. 후자의 경우 binding generation과 actor identity로 exact native
  unbind를 idempotent 제출하고 `RouteNotConnected`로 끝낸다. `:346-355`의 send 전에도 같은
  closed fence를 뒀다.
- 불변 근거: 정상 open 경로의 bind/send 순서와 timeout은 바꾸지 않았다. closed가 된 경우에만
  map 설치와 native send를 막으며, 기존 오류 분류(`RouteNotConnected`)를 쓴다.
- 테스트: `stream-runtime.test.js`에 “managed stream fences a native bind that completes after
  transport teardown” 1건 추가. native bind 중 close, exact generation unbind 1회, stale send
  거절을 확인했다. focused 148/148 통과(추가 전 전체), 최종 직접 계약 199/199 통과.
- 예상과 달랐던 점: 없음.

## 2. [H] routed bound-session generation 역전

- 변경: `runtime/host/remote-bound-session-relay.ts:114-143`에서 generation 있는 receive를 첫
  await 전에 map에 설치하고, target 갱신·rebind·local delivery의 각 await 뒤 캡처 generation이
  여전히 current인지 확인한다. command 44 commit도 `:549-555`에서 단조 갱신만 한다.
- 불변 근거: 같은/newer generation의 delivery 순서와 fallback 조건은 그대로다. 이미 더 큰
  generation이 설치된 오래된 frame만 rebind·delivery·fallback을 하지 않는다.
- 테스트: `stream-runtime.test.js`에 “routed bound-session receive does not regress a newer
  ownership generation” 1건 추가. 1세대 target update를 멈춘 동안 2세대를 완료시켜, rebind와
  delivery가 2세대만 수행됨을 확인했다. focused 149/149 통과, 최종 직접 계약 199/199 통과.
- 예상과 달랐던 점: 없음.

## 3. [M] remote actor packet stale target cache 및 capture fence

- 변경: `runtime/host/actor-packet-relay.ts:628-638`에서 request tenure key(Actor ID, object
  generation, node RID, binding/ownership/owner-lease generation)를 actor ref와 함께 캡처했다.
  원격 request await 뒤 `:749-766`에서 current Session/aggregate route의 tenure와 다시 대조해
  일치할 때만 reply target hint를 remember/clear한다. 원래 request의 captured response target을
  통한 terminal 처리는 유지했다.
- 보조 API: `runtime/host/remote-actor-packet-target-store.ts:64-90,175-197`에 expected tenure
  key 조건을 추가했다. current actor key가 다르면 state 및 세 cache를 전혀 바꾸지 않는다.
- 불변 근거: 원격 request의 route, one-way/request terminal, captured response target, timeout은
  바꾸지 않았다. 늦은 reply의 cache 반영만 현재 exact tenure로 제한했다.
- 테스트: `stream-runtime.test.js`에 “remote actor packet target rejects a reply captured for an
  older actor tenure” 1건 추가. ownership/lease generation이 바뀐 뒤 old expected key의
  remember/clear가 state/cache를 바꾸지 않음을 확인했다. focused 1/1 통과, 최종 직접 계약
  199/199 통과.
- 예상과 달랐던 점: 새 test의 fallback route가 mesh resolver를 요구해, local node RID를 사용해
  cache miss 자체가 route resolver에 들어가지 않도록 fixture만 보정했다. production 동작 변경은 없다.

## 4. [M] actor reclaim stale entry

- 변경: `runtime/locations/actor-location-claims.ts:290-324`에서 store resolve 뒤, stale
  delete/deactivate 전, takeover write 전, write 성공 row/generation 반영 전에
  `this.actors.get(canonical) === tracked` fence를 넣었다.
- 불변 근거: 현재 entry의 recovery/takeover 결과와 오류 집계는 그대로다. snapshot entry가 release와
  후속 claim으로 교체된 경우만 조용히 skip한다.
- 테스트: `location-runtime.test.js`에 “actor reclaim skips a tracked entry replaced while its Store
  read is pending” 1건 추가. resolve await 중 replacement를 설치해 replacement 보존 및 old
  deactivate 미호출을 확인했다. focused 1/1 통과, 최종 직접 계약 199/199 통과.
- 예상과 달랐던 점: 없음.

## 5. [M] Spot reclaim stale entry

- 변경: `runtime/locations/spot-location-claims.ts:241-319`에서 legacy resolve/takeover와 authority
  read/CAS의 각 await 뒤 및 delete/deactivate·row/storeVersion/generation terminal 반영 전에
  `this.spots.get(canonical) === tracked` fence를 넣었다.
- 불변 근거: legacy/authority의 정상 reclaim, CAS status 오류 집계, 동기 `trackInstanceAuthority`와
  `release` signature는 그대로다. 교체된 snapshot만 skip한다.
- 테스트: `location-runtime.test.js`에 “Spot reclaim skips a tracked entry replaced while its Store
  read is pending” 1건 추가. legacy resolve await 중 replacement를 설치해 replacement 보존 및 old
  deactivate 미호출을 확인했다. focused 1/1 통과, 최종 직접 계약 199/199 통과.
- 예상과 달랐던 점: 없음.

## 검증

- 매 수정 뒤 `flock -w 7200 /tmp/zlink-node-gate.lock npx tsc -b tsconfig.build.json --force` 통과.
- 최종 직접 계약: `flock -w 7200 /tmp/zlink-node-gate.lock node --test test/contract/stream-runtime.test.js test/contract/location-runtime.test.js` — 199/199 통과.
- 전체 gate: `flock -w 7200 /tmp/zlink-node-gate.lock npm run verify:m6c-runtime` — 규칙 §4에
  알려진 기존 실패 2건이 그대로 발생했다. 이번 수정의 focused/direct 계약은 모두 통과했으며,
  git 사용 금지에 따라 base checkout 대조는 하지 않았다.

전체 gate 집계 원문:

```text
1..115
# tests 115
# suites 0
# pass 113
# fail 2
# cancelled 0
# skipped 0
# todo 0
# duration_ms 7099.713194
```

기존 실패 원문:

```text
not ok 20 - remote Actor Join reports an incomplete legacy wire fence as ProtocolError
error: 'Missing expected rejection.'

not ok 88 - Session relocation retain identity includes every coordinator fence field
error: Expected "actual" to be strictly unequal to:
'7:9:actor-1:5:session:6'
```
