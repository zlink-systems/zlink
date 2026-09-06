# Java ready wait와 Session 교체 종료 수정 — D-108

승인된 F-R5-9·F-R5-13과 Spring monitor 테스트의 계약 불일치를 수정했다. 런타임 수정은
서로 다른 파일에 있어 독립 diff로 분리할 수 있다. Spring 테스트도 별도 diff다.
커밋과 sample runner 실행은 하지 않았다.

## Diff 1 — F-R5-9 호출 deadline

- 소유 계층: Framework의 request operation(`RequestCall`). Registry는 서버 선택과 admission 결과를 소유한다.
- Spec: channel messaging §3.2·§10, actor-model §8.1의 operation deadline 소유권, transport-liveness §2의 monotonic 시간 기준.
- 교차언어: `.NET` `ZLinkClientServerClientRuntime.cs:232`의 시작 시각·ready wait·잔여 timeout 전달과 대조했다. Kotlin의 `ZLinkOneWayCalls.kt:114`는 Java `ZLinkRequestCall.timeout/submit`에 위임하므로 별도 수정이 필요 없다.
- 변경 분류: **B — 기존 결함**, D-108 승인 범위.

수정 전 원인은 `ZLinkChannelRuntime.java:1074,1109`의 builder 생성 중 ready wait와
채널 기본 timeout 사용이다. `RequestCall`이 submit에서 `System.nanoTime()`으로 시간을
재고, 같은 서버 선택기에 남은 시간을 전달한다. Ready wait는 `min(remaining, 5 s)`이며,
transport request와 기존 완료 추적에는 대기에 소비한 시간을 뺀 값만 전달한다.
잔여 시간이 없으면 transport request를 시작하지 않는다.

별도 ClientServer request wrapper를 만드는 대안과 기존 `RequestCall`에 선택을 지연하는
대안을 비교했다. 기존 call이 deadline과 완료를 함께 소유하는 후자를 적용했다. Ready 여부에
따라 builder를 만드는 분기도 한 경로로 합쳤다.

변경 파일은 `framework/languages/java/` 기준이다.

- `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntime.java`
- `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelDirectCalls.java`
- `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java` — 대기 경계 주석
- `zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkClientServerReadyWaitTest.java` — 신규 회귀
- `zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntimeTest.java`
- `zlink-framework-core/src/test/java/systems/zlink/framework/runtime/ZLinkAsyncSubmitterTest.java`

신규 회귀는 150 ms 호출의 no-ready 종료, 늦은 READY 뒤 잔여 시간 전달, reply가 없을 때
원래 400 ms deadline에서의 종료, 채널 기본값보다 긴 호출에도 적용되는 5초 cap을 검증한다.
경과 시간은 모두 `System.nanoTime()`으로 측정한다.

기존 선택 테스트는 실제 request/reply와 `NotFound`를 검사한다. Descriptor 갱신을 기다리며
짧은 request를 반복하던 fixture는 record 처리·해제 완료를 기다리는 방식으로 바꿨다.
이 대기는 갱신 전 상태에서 시작한 probe의 deadline과 READY 전이가 겹치는 경쟁을 제거한다.
가중치 0·Serving 복귀·Draining의 선택 단언은 유지하고 성공 reply도 확인한다. 기본 timeout
테스트는 경과한 시간만 차감되는지와 원래 2초 deadline의 `DeadlineExceeded`를 함께 검사한다.

수정 전/후 규칙 수: 시간 예산 소유자 **2 → 1**, ready 여부에 따른 request 생성 경로 **2 → 1**.

## Diff 2 — F-R5-13 교체 connection 종료

- 소유 계층: Framework Session lifecycle. 실제 RID disconnect는 기존 backend transport가 수행한다.
- Spec: session-actor-binding §6의 교체 callback terminal 규칙과 §14의 100 ms 종료 검증 요구.
- 교차언어: `.NET` `ZLinkStreamSessionRuntime.cs:810`, Node `stream-session-runtime.ts:357`, C++ `stream_host_service.cpp:1774` 모두 100 ms 타이머에서 기존 close를 직접 시작한다. Kotlin은 Java Stream runtime을 공유한다.
- 변경 분류: **B — 기존 결함**, D-108 승인 범위.

수정 전 `ZLinkStreamRuntime.java:688` 이후 경로는 100 ms에 closing control을 보내고
25 ms 뒤 disconnect를 다시 예약했다. 현재는 retired identity를 확인한 같은 타이머 작업에서
closing control을 제출하고 바로 `disconnectPeer`를 호출한다. Control 송신 실패의 diagnostics는
기존 `sendControlAsync`가 담당한다. 추가 예약과 두 번째 identity 검사 helper를 제거했다.
기존 helper를 동기 호출하는 대안보다, 한 종료 단계에서 한 번 검사하는 구성을 선택했다.

변경 파일:

- `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java`
- `zlink-framework-core/src/test/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntimeIngressTest.java`

`boundSessionReplacementDisconnectsAtTheFirstTimerBoundary`는 closing control의 송신 stage가
완료된 경우와 계속 pending인 경우를 검사한다. 기존 executor에 관찰 probe만 연결하여 예약값
100 ms, callback terminal 이후의 monotonic 경과 시간, 다음 executor 작업 전에 시작한
transport disconnect, control 제출과 disconnect 사이의 20 ms 미만 간격을 확인한다.
실행 지연의 상한 2초는 scheduler 지연에만 적용된다. 예약값과 동일 작업 내 종료를 별도로
단언하므로 125 ms 정책이나 25 ms 후속 타이머를 허용하지 않는다. Peer의 control 처리로
발생하는 disconnect를 fixture가 대신 만들지 않는다.

수정 전/후 규칙 수: 예약된 교체 종료 단계 **2 → 1**, retired identity 확인 위치 **2 → 1**.

## 별도 test diff — Spring monitor 소비 계약

계약 소유자는 Core monitoring §2의 **open → recv → close**다. Framework monitoring §2는
관찰 책임을, transport-liveness §2는 raw monitor가 내부 신호라는 경계를 정한다.
변경 분류는 **B — pull 모델 전환 뒤 남은 테스트 결함**이다.

원인 commit은 `e65abaf7ac8b75b49ff4eefbe6bf05bd68d735c0`
(`framework/java: transition runtime to the 0.16.0 pull-completion model`, 2026-09-04)이다.
이 commit이 ClientServer의 `monitor.onEvent(...)`를 `ZLinkSocketMonitorDrainLoop.start(...)`로
바꾸고 Fake의 `onEvent`를 제거했으나 Spring 테스트에는 callback 기대값이 남았다.
현재 drain loop는 `ZLinkSocketMonitorDrainLoop.java:18`에서 `recv()`를 호출한다.

변경 파일:

- `zlink-framework-spring-boot-starter/src/test/java/systems/zlink/framework/spring/HostTest.java`
- `zlink-framework-spring-boot-starter/src/test/java/systems/zlink/framework/spring/ZLinkFrameworkAutoConfigurationTest.java`
- `zlink-framework-spring-boot-starter/src/test/java/systems/zlink/framework/spring/MonitorRecvProbe.java`

Fake 구현은 수정하지 않았다. 테스트 probe가 실제 Fake `recv()`의 반환을 관찰하고,
`recv → monitor close` 순서를 확인한다. Monitor open이 connect보다 앞서고 monitor close가
socket/context close보다 앞서는 기존 전체 호출 목록 단언도 유지한다. Fake에 없는 callback
호출을 추가하거나, 호출 목록 단언을 부분 포함 검사로 낮추지 않았다.

수정 전/후 규칙 수: monitor 소비 모델 **2(callback 기대값·pull 구현) → 1(pull)**.

## 검증 결과

전체 gate:

```bash
cd framework/languages/java
flock -w7200 /tmp/zlink-java-gate.lock ./gradlew test
```

**BUILD SUCCESSFUL, 실패 0, 오류 0.** Redis endpoint가 없어 생략된 live 검증은 임시 Redis를
기동하고 `ZLINK_REDIS_LOCATION_ENDPOINT`를 지정하여 같은 lock 아래
`:zlink-framework-locations-redis:test --rerun-tasks`로 추가 실행했다. Redis는 27건 모두
통과했고 임시 서버는 종료했다. 아래 표는 전체 gate와 Redis 추가 검증의 결과다.

| 모듈 | 테스트 | 실패 | skip |
|---|---:|---:|---:|
| framework-core | 1285 | 0 | 1 |
| framework-kotlin | 67 | 0 | 0 |
| framework-locations-redis | 27 | 0 | 0 |
| codec-msgpack / codec-protobuf | 2 / 1 | 0 | 0 |
| spring-boot-starter | 39 | 0 | 0 |
| testkit | 1 | 0 | 0 |
| http-client / http-client-kotlin | 44 / 14 | 0 | 0 |
| stream-connector | 124 | 0 | 0 |
| 합계 | 1604 | 0 | 1 |

- READY 신규 회귀 4건, 교체 종료 parameterized 회귀 2건, Spring 수정 테스트는 각각 **5회 통과**했다.
- 보강한 descriptor 선택·기본 deadline 테스트도 **5회 통과**했다.
- 수정 전 런타임을 사용한 회귀 확인에서는 READY 4건과 교체 종료 2건이 모두 실패했다.
- `git diff --check` 통과. 남은 실패는 없다.

남은 skip은 `ZLinkJavaRawMeshNodeShutdownSealTest.java:246`의 기존
`@EnabledIfEnvironmentVariable(named = "ZLINK_JAVA_STREAM_TRACE", matches = "1")` 조건이다.
이번 gate에서는 해당 환경 변수를 설정하지 않았다.

검증 로그와 XML 보관 위치:

- 전체: `/tmp/zlink-java-r5-full-test.log`, `/tmp/zlink-java-r5-full-results/`
- Redis live: `/tmp/zlink-java-r5-redis-live.log`
- 회귀 반복: `/tmp/zlink-java-r5-repeat-{1..5}.log`, `/tmp/zlink-java-r5-close-repeat-{1..5}.log`
- 선택·기본 deadline 반복: `/tmp/zlink-java-r5-selection-repeat-{1..5}.log`
- 수정 전 회귀: `/tmp/zlink-java-r5-regression-before.log`, `/tmp/zlink-java-r5-before-results/`
- Fixture 조사에 켠 기존 flow 로그: `/tmp/zlink-java-r5-selection.flow`. 조사용 로그 배선은 제거했다.
