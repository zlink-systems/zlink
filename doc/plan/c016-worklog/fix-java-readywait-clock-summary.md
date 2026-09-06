# Java ReadyWait clock 검증 결과

감독이 Java 내부 주입 변경과 검증 결과를 판정하기 위한 기록이다. `ZLinkClientServerReadyWaitTest`의 실제 경과 시간 단언을 제어 가능한 monotonic 시각의 정확한 경계 단언으로 바꿨다. CPU 부하 20개 아래 클래스 **20/20회**, 전체 `gradlew test` **실패 0건**이다. Public API와 운영 동작을 유지했으며 commit은 하지 않았다.

## 원인과 소유권

- 원인: 수정 전 `ZLinkClientServerReadyWaitTest.java:43,56,78,96`은 테스트 스레드에서 실제 시간을 측정했고, `:63,87`은 scheduler 지연을 포함한 값을 제한했다. Operation은 `ZLinkChannelDirectCalls.java:313`에서 별도로 시작 시각을 정했다. 기존 실패 근거는 `zlink-work/c016/gates/java-r8-gate.log:46–50`이다.
- 소유 계층: Framework `RequestCall`이 호출 시간 예산을 소유한다. Registry는 기존 ready 선택·대기를 수행하며, `ZLinkChannelCallRuntime.track`은 전달받은 잔여 시간으로 기존 완료 타이머를 예약한다.
- Spec 조항: `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:170–181` §3의 `min(call timeout, 5 s)`와 Java exact interface `languages/java/interfaces/channel-messaging.ko.md:148–154`; D-093의 operation 단일 deadline 소유권과 `02-channel-transport/05-transport-liveness.ko.md:70–74` §2/D-095의 monotonic 시간 기준을 유지한다.
- 교차언어 대조: `framework/languages/node/test/contract/client-server-location-runtime.test.js:1807–1842`는 `performance.now`와 timer를 제어해 정확한 잔여 시간을 검증한다. Java는 직접 호출하던 `System.nanoTime()`을 내부 생성자 의존성으로 주입한다. Java에만 변경이 필요한 이유는 정적 시간원과 blocking 대기·예약 방식의 구조적 차이다.
- 변경 분류: **B — 기존 테스트 결함**. 이번 작업 지시가 승인한 내부 seam 추가이며 timeout 정책이나 오류 분류를 바꾸는 런타임 결함 수정은 없다.
- 수정 전/후 규칙 수: 운영 deadline 소유자 **1 → 1**, 실제 경과 시간의 허용 구간 단언 **5 → 0**. 새로운 deadline 상태·재시도·timeout 예외 규칙은 없다.

## 변경 파일과 주입 지점

아래 source 위치의 기준은 `framework/languages/java/zlink-framework-core/src/`다.

| 파일:행 | 변경 |
|---|---|
| [ZLinkChannelCallRuntime.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelCallRuntime.java):74,89 | Package-private 생성자에 `LongSupplier`를 추가하고 operation이 사용할 `nanoTime()`을 제공한다. 기존 생성자의 기본값은 `System::nanoTime`이다. 기존 `ScheduledExecutorService` 주입을 사용한다. |
| [ZLinkChannelDirectCalls.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelDirectCalls.java):313,339,352–353 | 시작 시각, ready 선택 전·후 잔여 시간, duration metric을 같은 주입 시간원으로 읽는다. 계산식과 분기는 유지한다. |
| [ZLinkChannelSocketRegistry.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java):99,277 | Ready 대기의 시간원과 `parkNanos`를 내부 생성자로 받는다. 기존 생성자는 `System::nanoTime`·`LockSupport::parkNanos`를 사용하며 기존 5 ms poll 상한을 유지한다. |
| [ZLinkChannelRuntime.java](../../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntime.java):431,441 | Package-private 생성자에서 같은 시간원을 call runtime과 registry에 연결하고 대기 함수·timeout executor를 받는다. 모든 기존 public 생성자는 기존 monotonic source, park, 이름이 같은 daemon single-thread scheduler를 사용한다. |
| [ZLinkClientServerReadyWaitTest.java](../../../framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/channels/ZLinkClientServerReadyWaitTest.java):41–126,233 | 기존 timing test 네 개를 정확한 가상 시각으로 바꾸고 cap에서 admission되는 경우를 추가한다. 테스트 전용 `ManualTime`이 clock, 대기 진행, one-shot 예약을 함께 소유한다. |

Clock만 주입하고 실제 park·완료 scheduler를 유지하는 대안은 clock을 진행할 스레드 순서와 실제 타이머에 계속 의존한다. 선택한 구성은 같은 테스트 시간으로 기존 대기 함수와 예약 작업을 실행한다. Production deadline 알고리즘, cap 계산, transport 제출 경로는 그대로 사용한다.

## 테스트 경계

- Builder 생성은 readiness 대기를 시작하지 않는다. 생성 뒤 가상 시간을 1초 진행하고 제출해도 150 ms 호출은 **submit + 150 ms**에서 `UNAVAILABLE`이며 business request는 0건이다.
- 400 ms 호출을 기다리는 중 **+150 ms**에 admission한다. Business request 시작은 정확히 +150 ms이고 전달 timeout은 **250 ms**, 둘의 합은 원래 **400 ms**다.
- Reply를 완료하지 않은 호출은 원래 deadline **1 ns 전**까지 pending이다. 마지막 1 ns를 진행하면 기존 `track` callback이 정확히 **+400 ms**에서 `DEADLINE_EXCEEDED`로 완료한다.
- Channel 기본값 150 ms, 호출 timeout 8초에서 ready가 없으면 정확히 **+5초**에 `UNAVAILABLE`, business request는 0건이다.
- 같은 설정에서 **+5초**에 admission하면 정확히 **3초**를 request에 전달한다.
- 실제 시간 대기는 fixture의 기존 1초 ordering guard만 남는다. Monitor의 다음 receive를 기다려 admission callback 등록이 끝난 뒤 가상 시간을 진행한다(`:189–194,215–216`). 실제 소요 시간은 단언하지 않는다.

## 검증 결과

모든 Gradle 실행은 `/tmp/zlink-java-gate.lock`을 독점 `flock`으로 잡았다. 첫 focused 실행은 CLI `flock`, 부하 반복과 전체 gate는 Python `fcntl.flock(LOCK_EX)`으로 같은 lock을 유지했다.

| 검증 | 결과 |
|---|---|
| 최초 focused `./gradlew --no-daemon :zlink-framework-core:test --tests systems.zlink.framework.runtime.channels.ZLinkClientServerReadyWaitTest --rerun` | 5 tests, 실패·error·skip 0 |
| 같은 focused 명령 20회, `yes > /dev/null` 프로세스 20개 유지 | **20/20회**, 총 100 tests, 실패·error·skip 0, 재시도 0 |
| 전체 `./gradlew --no-daemon test --rerun` | exit 0, **1621 tests / 0 failures / 0 errors / 15 existing skips**, 56.717초 |
| `git diff --check -- framework/languages/java` / production diff의 public 선언 비교 | 통과 / 변경 없음 |

부하 구간은 `2026-09-06T05:36:41Z`부터 `2026-09-06T05:44:38Z`까지다. 매 실행 전·후에 worker 20개가 모두 실행 중임을 확인했고, 측정한 1분 load average 최대는 **24.18**였다. 반복 실행 총 소요 시간은 **477.401초**다. 종료한 부하 PID가 남아 있지 않음을 확인한 뒤 전체 gate 결과를 확인했다.

Gate의 기존 skip은 `ZLINK_JAVA_STREAM_TRACE=1` 조건의 `ZLinkJavaRawMeshNodeShutdownSealTest` 1건과 `ZLINK_REDIS_LOCATION_ENDPOINT` 미설정에 따른 Redis live test 14건이다. 이번 변경에서 skip이나 조건 완화는 추가하지 않았다.

증거 디렉터리: `framework/languages/java/zlink-framework-core/build/readywait-clock-verification/`.
`results.json`에 각 명령·exit code·worker 수·load average·XML 집계가 있다. `focused.log/xml`, `load-01.log/xml`부터 `load-20.log/xml`, `full-gate.log`, `full-gate-xml/<module>/TEST-*.xml`을 보존했다.

## BLOCKERS

없음. 요청한 클래스 부하 검증과 전체 gate가 통과했다. 전체 gate에서 실행되지 않은 기존 환경 조건 테스트 15건은 위에 명시했다.
