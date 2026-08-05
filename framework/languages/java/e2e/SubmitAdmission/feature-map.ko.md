# Java·Kotlin SubmitAdmission E2E feature map

기준 문서는
[`Config 13 — One-way submit admission`](../../../../doc/framework/common/e2e/config-13-submit-admission.ko.md)이다.
Runner는 Caller, Mesh target과 classic fanout publisher를 서로 다른 process로 실행한다. `부분 구현`은 public
결과와 process 순서 일부를 검증하지만 공통 scenario가 요구하는 gate나 내부 observer가 모두 연결되지 않았다는
뜻이다. 이 상태는 해당 scenario의 완료 증거가 아니다. 미구현 selector를 직접 지정하면 성공으로 건너뛰지
않고 종료 코드 3을 반환한다.

| 시나리오 | 상태 | 근거 또는 blocker |
|---|---|---|
| SA-E2E-01 | 부분 구현 | Ready remote Node direct와 ChannelName call을 public `submit()` 한 번으로 실행해 `SUBMITTED`와 target handler 1회를 확인한다. 다른 family와 scheduler enqueue·transport attempt·commit observer는 없다. |
| SA-E2E-02 | 미구현 | `ReceiverGate`와 destination별 SEND_READY signal·retry observer가 필요하다. |
| SA-E2E-03 | 미구현 | Pending capacity를 고정하는 fixture와 첫 transport attempt observer가 필요하다. |
| SA-E2E-04 | 부분 구현 | Focused JVM runtime test는 1ns를 1ms로 올림하고 timeout 뒤 exact destination ready signal을 보내도 transport·local admission attempt가 늘지 않는지 확인한다. Process `ReceiverGate`와 family별 late delivery evidence는 남아 있다. Timeout을 늘리거나 call을 반복 submit하지 않는다. |
| SA-E2E-05 | 부분 구현 | Typed expected-RID registry에 없는 RID를 100회 `REQUEST_TARGET_NOT_FOUND`로 확인한다. 같은 registry의 target process를 종료하고 public runtime snapshot에서 ready 해제를 확인한 뒤 100회 `ROUTE_NOT_CONNECTED`로 구분한다. 독립 connection gate와 native teardown observer는 없다. 승인 package process evidence는 2026-08-03 실행에서 두 결과를 각각 100회 확인했다. |
| SA-E2E-06 | 미구현 | Drain admission barrier와 source process 종료 뒤에도 결과를 보관하는 collector가 필요하다. |
| SA-E2E-07 | 미구현 | JVM cancellation winner와 Logical Multicast commit 전·후 barrier가 필요하다. |
| SA-E2E-08 | 부분 구현 | Self RID는 기존 Mesh application dispatcher의 bounded local queue를 사용한다. Focused test는 queue 포화 뒤 Node destination capacity signal 한 번으로 pending call을 수락하고, timeout 뒤 signal에는 재시도하지 않음을 확인한다. Process gate에서 local·remote pending·deadline evidence를 나란히 비교하는 절차는 남아 있다. |
| SA-E2E-09 | 부분 구현 | Caller weight 0, remote target weight 100인 ChannelName call이 positive-weight target에서 한 번 처리되는지 확인한다. Pending deadline과 선택 member observer는 없다. |
| SA-E2E-10 | 미구현 | ClientServer client·server process와 DEALER admission evidence가 필요하다. |
| SA-E2E-11 | 미구현 | Spot generation, stale handle과 local·remote mailbox gate가 필요하다. |
| SA-E2E-12 | 미구현 | Actor generation, stale handle과 local·remote mailbox gate가 필요하다. |
| SA-E2E-13 | 미구현 | Queue-less executor direct handoff와 publish snapshot barrier가 필요하다. |
| SA-E2E-14 | 부분 구현 | Subscriber process가 없는 상태에서 classic fanout public call 한 번이 `SUBMITTED`인지 확인한다. 이후 manual subscriber를 시작해 같은 message의 late delivery가 0인지 확인한다. Transport attempt·commit observer는 없다. |
| SA-E2E-15 | 미구현 | Bound session·Actor process, binding generation과 local·remote gate가 필요하다. |
| SA-E2E-16 | 미구현 | STREAM server session·connector peer와 wire sequence evidence가 필요하다. |
| SA-E2E-17 | 미구현 | 실제 request reply token과 concurrent terminator barrier가 필요하다. |
| SA-E2E-18 | 미구현 | Direct generation과 select-one first-attempt member observer가 필요하다. |
| SA-E2E-19 | 미구현 | Timeout·shutdown terminal 뒤 connection generation과 late attempt observer가 필요하다. |
| SA-E2E-20 | 부분 구현 | Remote Node handler gate가 닫힌 상태에서 `SUBMITTED`가 먼저 완료되고 handler completion은 gate 해제 뒤 한 번인지 확인한다. 나머지 family matrix는 없다. |
| SA-REG-01 | 구현 | 공통 verifier로 제거한 동기 terminator가 public contract와 구현에 남지 않았는지 확인한다. |
| SA-REG-02 | 구현 | Java test가 최초 `DONT_WAIT`, source/HWM 기반 bounded pending, exact SEND_READY·local capacity one-signal/one-retry, timeout·shutdown cleanup, cancellation과 중복 submit을 검증한다. Router HWM·send timeout 전달, Actor direct의 polling 없는 typed result, Self RID payload 독립 소유·handler 비대기·missing·sealed 결과와 handler 예외 뒤 drain claim 정리도 함께 검증한다. |
| SA-REG-03 | 구현 | Kotlin coroutine cancellation이 Java `CompletionStage.cancel(false)`로 전달되는지 검증한다. |
| SA-REG-04 | 부분 구현 | Internal admission runtime에서 disposal과 exact ready signal을 100회 같은 barrier에서 경쟁시켜 terminal·cleanup이 각각 한 번이고 late ready 뒤 cleanup이 늘지 않음을 확인한다. Process별 waiter·reservation·callback observer와 `SA-REG-04.b` evidence는 남아 있다. |

## 실행

```bash
ZLINK_LOCAL_PACKAGE_ROOT=/path/to/candidate \
ZLINK_CORE_PACKAGE_PREFIX=/path/to/approved/zlink-core/11.1.0 \
ZLINK_CORE_PACKAGE_EVIDENCE=/path/to/core-package-20260801.json \
  ./framework/languages/java/e2e/SubmitAdmission/run_e2e.sh all

./framework/languages/java/e2e/SubmitAdmission/run_e2e.sh SA-E2E-02
# 미구현 selector이므로 exit 3
```

runner는 `SA-REG-02`와 `SA-REG-03`을 실행하기 전에 isolated Maven root를 Gradle에 직접 지정한다.
현재 `gradle/libs.versions.toml`의 `zlinkBindings` 버전이 각 module의 `testRuntimeClasspath`에서
정확히 한 번 resolve되고, resolved artifact의 SHA-256이 입력한 candidate와 같은지도 확인한다.
또한 `ZLINK_CORE_PACKAGE_PREFIX`와 `ZLINK_CORE_PACKAGE_EVIDENCE`가 가리키는 승인된 Core package의
version·runtime hash·provenance와 binding jar의 embedded runtime을 비교한다. shared package,
이전 artifact 또는 `core/build`의 다른 runtime이 선택되면 test를 시작하지 않는다.

`all`은 process selector `SA-E2E-01·05·08·09·14·20`과 `SA-REG-01~03`을 실행한다. Process 완료 gate는
`SA-E2E-01~20`과 `SA-REG-01~04`가 모두 구현되고 독립 evidence를 남긴 뒤에만 충족한다.
# 2026-07-29 Object Client 연결 계약 검증

Java와 Kotlin이 공유하는 Framework-owned raw RouteMesh에서 다음 경계를 focused test로
검증했다.

- Automatic planner는 양쪽 모두 Object Client이고 RouteMesh Channel Server membership도
  없는 pair만 연결 대상에서 제외한다.
- Manual handshake는 같은 pair를 `NOT_REQUIRED`로 끝내고 reconnect announcement와
  liveness 대상에서 제외한다.
- 어느 한쪽에 weight `0`인 RouteMesh Channel Server membership이 있으면 연결한다.
- Object Client RID를 Node direct target으로 사용하면 `NOT_FOUND`로 끝낸다.

검증 명령은 `ZLinkAutoConnectPlannerTest`와 `ZLinkJavaRawMeshNodeM6ATest`를
single worker로 실행했다. 결과는 13/13 성공이며 실패·오류·skip은 0이다. Automatic과
Manual의 전체 process E2E는 아직 남아 있으므로 `SA-E2E-08`의 상태는 부분 구현을 유지한다.
