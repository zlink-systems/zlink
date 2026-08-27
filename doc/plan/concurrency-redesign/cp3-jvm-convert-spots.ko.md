# CP3 JVM spots 상태 보호 전환 보고

## 결론

지정된 spots 계열 R 취득 25곳을 state lane으로 전환했다. 대상 일곱 파일의
`synchronized` token은 **25 → 0**이다. `E`/`S` 분류 파일은 변경하지 않았고,
`ZLinkSpotRetireControl`의 2026-08-27 publish placeholder 선설치도 유지했다.

| 파일 | synchronized 전후 | 분류 · 그룹 판정 · lane 편성 | 재진입 실측·해소 |
|---|---:|---|---|
| `runtime/spots/ZLinkSpotPublisherRuntime.java` | 7 → 0 | runtime의 `nodesByChannel`·`spotsByChannel`·`closed`는 C2 한 그룹(`stateLane`, 평범한 `HashMap`), 각 `MulticastFuture`의 commit/cancel/payload-release는 독립 C2 한 그룹 | outer public 표면은 `...Core` 읽기로 분리했다. `MulticastFuture` 완료는 lane 밖에서 실행해 완료 dependent가 lane에 거짓 재진입하지 않는다. handoff 경로는 native submit 경계를 지난 뒤 admission을 완료해 bounded handoff의 기존 관측을 보존했다. |
| `runtime/spots/ZLinkDefaultSpotContext.java` | 4 → 0 | `relocationBarrier`와 `relocationReadyWaiter`는 동일 relocation C2 그룹(`relocationStateLane`) | readiness claim은 lane에서 반환 전 등록하고, boundary/cancellation은 waiter를 lane에서 detach한 뒤 callback·future 완료를 lane 밖에서 처리한다. |
| `runtime/spots/ZLinkSpotRetireControl.java` | 3 → 0 | Target의 `slots`와 Slot의 `staged`/`published`/`aborted`는 C2 한 그룹(`stateLane`, 평범한 `HashMap`) | stage/publish/abort public 경로는 lane core로 상태만 전이한다. endpoint 호출과 completion은 turn 밖이며, publish는 placeholder를 endpoint publish 전에 lane에서 설치한다. |
| `runtime/internal/handlers/ZLinkActorHandlerInstances.java` | 3 → 0 | Actor identity registry는 C1 (`STATE_LANE`, `IdentityHashMap`) | owner lookup만 lane turn에서 끝내고 `owner.instance` 호출은 turn 밖으로 분리했다. |
| `runtime/internal/handlers/ZLinkHandlerInstanceOwner.java` | 2 → 0 | `closed`와 handler instance map은 activation lifecycle C2 한 그룹(`stateLane`, `LinkedHashMap`) | close는 소유 인스턴스를 lane에서 detach한 뒤 destroy/activation close를 turn 밖에서 수행한다. |
| `runtime/host/ZLinkRouteMeshRuntimeView.java` | 3 → 0 | 각 `SignalHub`의 signal 목록·stop/pump 상태는 C2 한 그룹(허브별 `stateLane`, `ArrayList`) | signal snapshot/detach는 lane에서, `publisher.signal()`과 monitor thread 시작·interrupt는 turn 밖에서 수행한다. |
| `runtime/host/ZLinkFrameworkRuntime.java` | 3 → 0 | core HWM snapshot·reset epoch·cached status는 C2 한 그룹(`capacityStateLane`); `coreHwmContextActive`는 C3 `AtomicBoolean` 유지 | reset/status/close capture가 같은 capacity turn에서 실행된다. 외부 backend·queue 호출을 포함하는 기존 동기 구간은 반환 전 완료하는 동기 state-lane bridge로 보존했다. |

## 발견 5·6·7·9·10 대조

- 발견 5: lane 내부에서 `CompletableFuture.complete`하지 않았다. publisher multicast, relocation readiness, retire stage/publish의 완료는 모두 state turn 밖에서 신호한다.
- 발견 6: handler destroy, SignalHub publisher signal, retire endpoint 호출은 상태 전이/placeholder 설치 뒤 turn 밖으로 분리했다.
- 발견 7: 대상에 외부 `await`를 품는 작업 프로토콜 monitor는 없었다. `E`/`S` 취득은 변경하지 않았다.
- 발견 9: readiness waiter와 retire slot/placeholder는 `in...StateLane(...).join()`으로 호출자 반환 전에 등록·캡처한다. capacity reset/status도 기존 동기 표면을 유지했다.
- 발견 10: route-mesh SignalHub의 signal snapshot, retire Slot의 staged/published/aborted 판정, capacity HWM snapshot과 epoch/status 조립은 각각 한 lane turn 안에서 함께 읽었다.

## 본문 조정 목록

없음. `framework/doc/**`와 spec은 수정하지 않았다.

## 테스트 결과

실행 명령:

```text
cd framework/languages/java
flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test
```

Gradle 집계 원문:

```text
BUILD SUCCESSFUL in 27s
10 actionable tasks: 2 executed, 8 up-to-date
```

JUnit XML 집계: `tests=1149 failures=0 errors=0 skipped=0`.

## STOP 여부와 예상과 달랐던 점

STOP 없음. full run의 알려진 세 flake는 발생하지 않았다.

예상과 달랐던 점은 `ZLinkSpotPublisherRuntime`의 bounded handoff test가 첫 전환에서
queued admission 완료와 native submit 시작 사이의 scheduling race를 드러낸 점이다.
일반 executor 직접 제출은 종전처럼 source-local admission을 먼저 완료하고, bounded handoff
제출만 native submit 경계 뒤에 완료하도록 유지해 기존 test의 worker-capacity 관측을 보존했다.
