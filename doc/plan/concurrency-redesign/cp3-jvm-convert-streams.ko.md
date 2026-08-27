# CP3 JVM streams 상태 보호 전환 보고

## 결과

대상 R 취득 19개를 state lane으로 전환했다. `synchronized`는
`ZLinkStreamRuntime` 7개에서 0개, `ZLinkRouteMeshRuntimeService` 12개에서 0개가 됐다.
E/S 분류 취득은 변경하지 않았고, spec 파일도 수정하지 않았다.

## 파일별 판정

| 파일 | synchronized 전/후 | C1 | C2 | C3 | 그룹 판정과 lane 편성 |
|---|---:|---|---|---|---|
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java` | 7 / 0 | 없음 | 수신 루프의 `receiveStates`·`ignoredPeers`·cursor·close/registration 상태, peer별 `buffer`·pending frame·close 상태 | 기존 `sessionContexts`와 close metric flag는 그대로 atomic/concurrent 구조 | 수신 루프당 lane 1개와 peer 수신 상태당 lane 1개다. C2 collection은 모두 평범한 `HashMap`/`HashSet`으로 두고, map에서 peer state를 detach한 뒤 frame close와 socket/handler 작업을 lane 밖에서 수행한다. |
| `framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/internal/runtime/ZLinkRouteMeshRuntimeService.java` | 12 / 0 | 기존 service-level `monitorHubs`/`sequences` concurrent registry는 범위 밖으로 유지 | MonitorHub의 observer·signal·stop·pump 상태, ObserverSubscription의 pending queue·cancelled 상태 | demand/draining은 기존 `AtomicLong`/`AtomicInteger` 유지 | MonitorHub당 private FIFO lane 1개, subscription당 private FIFO lane 1개다. JPMS가 core internal `ZLinkStateLane`을 export하지 않으므로, 동일한 비재진입·FIFO·`completeAsync` 완료 규칙의 private `StateLane`을 이 대상 파일 안에 두었다. |

`StreamReceiveLoop`의 map membership과 peer buffer는 parent/child lane으로 경계를 나눴다. parent turn은 membership을 먼저 detach하고, child turn은 close flag와 pending frame을 terminal로 만든다. snapshot을 이미 보유한 수신 루프도 child close flag만 관찰하므로 두 그룹 사이에 mutable authorization이 남지 않는다.

## 재진입과 완료 규칙

- MonitorHub의 `onSubscribe`, signal, observer terminal callback은 lane 밖으로 이동했다. lane 안에서는 observer/signal 배열을 capture 또는 detach한 뒤 private `...Core()`만 호출한다.
- subscription의 `onNext`와 stream frame close도 lane 밖에서 실행한다. lane 안에서 같은 public 표면을 다시 호출하지 않는다.
- 발견 5: core `ZLinkStateLane`의 `completeAsync`를 그대로 사용했고, spring private `StateLane`도 result completion을 `completeAsync`로 게시한다. pending-session producer completion은 기존처럼 state turn 뒤에 남겨, 2026-08-27 same-producer 재진입 즉시 실패 보호를 되돌리지 않았다.
- 발견 9: 모든 새 `inReceiveStateLane`/`inStateLane` 호환 경계는 `join()`으로 완료를 기다린다. peer 등록·detach, observer 등록·terminal detach, queue poll이 caller 반환 전에 완료된다.
- 발견 10: 수신 peer 선택은 sorted state snapshot과 시작 cursor를 한 turn에서 capture하고, notification의 ignored 판정과 state detach 및 일반 frame의 closed/ignored/map lookup-or-create를 각각 한 turn에 묶었다. subscription도 cancelled·demand·pending poll을 한 turn에서 판정한다.

## 본문 조정 목록

없음. 테스트 기대값과 공개 API, 상태 순서·timeout·오류 코드는 조정하지 않았다.

## 검증

JVM 트리 lock을 사용했고 Java/Kotlin Gradle 실행을 병렬로 수행하지 않았다.

| 명령 | 결과 집계 원문 |
|---|---|
| `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-core:test` | `BUILD SUCCESSFUL in 29s` / `10 actionable tasks: 2 executed, 8 up-to-date` |
| `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-spring-boot-starter:test --tests systems.zlink.framework.spring.internal.runtime.ZLinkRouteMeshRuntimeServiceTest` | `BUILD SUCCESSFUL in 1s` / `18 actionable tasks: 1 executed, 17 up-to-date` |
| `flock -w 10800 /tmp/zlink-jvm-gate.lock ./gradlew :zlink-framework-spring-boot-starter:test` | `39 tests completed, 1 failed` / `BUILD FAILED in 5s` (`ZLinkMicrometerMetricSinkTest > exportsMetricOnlyApplicationJobQueuePressureAccounting`) |
| 위 Micrometer test의 올바른 FQCN 단독 재실행 | `BUILD SUCCESSFUL in 2s` / `18 actionable tasks: 1 executed, 17 up-to-date` |

Micrometer 실패는 대상 파일과 무관하며 단독 재실행에서 통과했다. 도중 package를 잘못 지정한 1회는 Gradle의 `No tests found for given includes`였고, 소스/테스트 실패로 분류하지 않았다. 지정된 알려진 full-run flake 3건은 발생하지 않았다.

## STOP 여부와 예상 밖 항목

- STOP: 없음. 관측 동작을 바꾸지 않고 각 C2 경계를 lane ownership으로 닫을 수 있었다.
- 예상과 달랐던 점: spring starter는 core의 internal execution package를 JPMS로 접근할 수 없었다. module export를 넓히지 않고 대상 파일의 private lane으로 동일 보장을 구현했다.
