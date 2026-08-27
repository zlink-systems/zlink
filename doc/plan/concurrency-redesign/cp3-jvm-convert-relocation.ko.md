# CP3 JVM relocation 상태 보호 lane 전환

## 범위와 판정

감사표 §3의 R 취득만 대조했다. 대상 다섯 파일의 전환 전 `synchronized`
수는 각각 14, 8, 7, 6, 3으로 합계 38이고, 전환 후에는 모두 0이다.
`E` 실행 primitive 및 `S` socket/dispose 작업 프로토콜은 수정하지 않았다.

## 파일별 결과

### `ZLinkCanonicalRelocationStateMachine.java`

- `synchronized`: 14 -> 0.
- 분류: relocation attempt map 네 개와 target publication/relay accounting은 C2,
  `openSourceQuiescenceWindows`는 C3 `AtomicInteger`로 유지했다.
- 편성: machine lane 하나가 `sources`, `targets`, `terminalTargets`,
  `retainedSources` 평범한 `HashMap` 네 개를 함께 소유한다. source relay batch,
  target relay boundary, target attempt는 각 attempt가 소유하는 별도 C2 lane이다.
  map membership과 attempt 제거에는 exact object identity를 함께 확인한다.
- 재진입: public 표면을 lane turn 안에서 다시 부르는 경로는 없었다. READY와 target
  publish의 이전 monitor 재진입 지점은 `PublicationClaim` private claim core로
  분리하여 placeholder-first claim을 한 turn에서 끝낸다.
- 발견 5: lane work의 반환은 `ZLinkStateLane.runAsync`의 `completeAsync` 완료
  경로를 사용한다. 본 파일의 publication completion은 lane scope 밖에서 수행된다
  (`publishReady` 520행, `publishTarget` 950행).
- 발견 9: attempt/map 등록, placeholder claim, retained copy 등록은 모두
  `inStateLane(...).join()`으로 호출 반환 전에 끝난다 (77-81행, 524-529행,
  954-958행).
- 발견 10: CUTOVER 검증의 `recordCount`와 `checksumCrc32c`를
  `RelayBoundary.Snapshot` 하나로 같은 turn에서 캡처했다 (922-925행).
- 본문 조정 목록: 없음.

### `ZLinkStandaloneActorRelocationSourceBuilder.java`

- `synchronized`: 8 -> 0.
- 분류: `PreparedSource`의 retained commit, final journal, capture/commit/terminal
  flags 및 forward binding은 하나의 C2 그룹이다.
- 편성: `PreparedSource.stateLane` 하나가 그 그룹을 소유한다 (868행).
  retain/capture claim과 final journal capture는 동기 lane turn으로 완료한 뒤 relay와
  Session abort 같은 외부 비동기 작업을 turn 밖에서 계속한다.
- 재진입: 외부 completion에서 다시 상태를 끝내는 `finish`를 private lane helper로
  분리했다. 같은 객체 public surface의 lane 내부 재호출은 없다.
- 발견 5: source queue `complete()`는 state turn 뒤에 호출한다
  (1092행 이후). lane 내부에서 caller dependent를 완료하지 않는다.
- 발견 9: relay boundary retain과 final journal capture는 반환 전에 lane join으로
  끝난다 (979행, 1014행).
- 발견 10: retained cut으로부터 만든 final journal 전체를 한 lane turn에 저장한다.
- 본문 조정 목록: 없음.

### `ZLinkStandaloneActorRelocationStagingOwner.java`

- `synchronized`: 7 -> 0.
- 분류: staged ingress 두 목록, durable/direct backlog, published/lifecycle/terminal
  flags는 한 C2 그룹이다.
- 편성: `Staged.stateLane` 하나가 ingress close, drain-once, replay completion,
  discard를 소유한다. backlog의 `consumed`와 direct replay의 `replayed`도 같은
  staged lane에 넣어 별도 collection lock으로 분리하지 않았다.
- 재진입: ingress callback/replay 및 backend discard는 lane 밖에서 실행하고, callback
  전의 상태 전이와 pending ingress capture만 turn에서 끝낸다.
- 발견 5: lane completion은 `ZLinkStateLane` 비동기 completion을 사용한다.
- 발견 9: close/accept/relayed-record/drain-once 결정은 각 public 반환 전에 동기
  lane join으로 확정한다 (85행, 150행, 164행, 187행).
- 발견 10: close 시 relayed와 temporary ingress를 한 turn에서 함께 snapshot/clear한다
  (85-107행).
- 본문 조정 목록: 없음.

### `ZLinkUserSpotAggregateStagingOwner.java`

- `synchronized`: 6 -> 0.
- 분류: aggregate staged ingress, durable backlog, spot/actor publication, lifecycle,
  terminal flags는 하나의 C2 그룹이다.
- 편성: `Staged.stateLane` 하나가 ingress close, publication readiness, drain-once,
  discard capture를 소유한다. actor/spot collection을 concurrent container로
  대체하지 않았다.
- 재진입: backend ready/replay/discard와 ingress callback은 lane 밖에 두고, 그 전후의
  state transition은 private lane work로 분리했다.
- 발견 5: `completeRelocationReady` 후의 `backlogSealed` 기록은 별도 lane turn에서
  완료한다 (209행); lane scope에서 future를 직접 complete하지 않는다.
- 발견 9: close backlog, ingress admission, relay admission, consumed claim은 반환 전에
  lane join으로 확정한다 (188행, 276행, 302행, 359행).
- 발견 10: close backlog의 relayed/temporary snapshot과 clear는 한 turn이다
  (188-207행).
- 본문 조정 목록: 없음.

### `ZLinkUserSpotRetireSourceBuilder.java`

- `synchronized`: 3 -> 0.
- 분류: unresolved preparation 목록은 단일 registry C1이며, 기존 `PreparedSource`의
  relay/commit/terminal state는 C2다.
- 편성: C1은 `UnresolvedPreparations.stateLane`이 평범한 `ArrayList`를 소유한다
  (1550행). C2 `PreparedSource.stateLane`은 기존 group을 계속 소유한다.
- 재진입: unresolved recovery callback은 목록 snapshot 뒤에 turn 밖에서 실행하고,
  add/remove/remember/forget만 registry private lane core로 보낸다. PreparedSource도
  public 표면 재호출 없이 private `inStateLane` core만 호출한다.
- 발견 5: relocation commit claim의 dependent completion은 기존처럼
  `completeAsync`로 lane scope 밖에 낸다 (1210행 이후).
- 발견 9: unresolved snapshot/count/add/remove 및 PreparedSource retain/final-journal
  capture는 모두 반환 전 lane join으로 끝난다 (275행, 279행, 1166행, 1228행).
- 발견 10: unresolved recovery는 snapshot 한 번으로 대상 목록을 고정하며, final journal은
  capture cut 전체를 한 turn에 기록한다.
- 본문 조정 목록: 없음.

## 테스트 결과

실행 명령:

```text
cd framework/languages/java
flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test
```

집계 원문:

```text
1162 tests
0 failures
0 skipped
26.901s duration
```

대상 단위 클래스 여섯 개만 지정한 focused gate도 별도로 통과했다.

## STOP 및 예상과 달랐던 점

- STOP: 없음. 관측 순서, timeout, error code 및 test expectation을 바꾸지 않았다.
- 예상과 달랐던 점: canonical machine의 map 네 개도 C2 ownership region이므로
  `ConcurrentHashMap`을 유지하는 대신 평범한 `HashMap` 네 개를 machine lane 하나로
  함께 옮겨야 했다. CUTOVER checksum/count는 독립 getter 두 번이 아니라 한 snapshot
  turn이어야 했다.
