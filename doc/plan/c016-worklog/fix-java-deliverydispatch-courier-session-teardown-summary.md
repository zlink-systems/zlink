# Java DeliveryDispatch CourierSession 종료 수정 결과

2026-09-05. CourierSession의 종료 실패 원인은 **실패한 unbind 뒤에도 local binding이
남아 socket close가 같은 unbind를 다시 제출하는 것**이다. Java Framework의 binding
정리 지점에서 수정했다. 종료 실패의 단계·예외·내부 원인은 기존 runtime 상태 로그의
최종 종료 event 한 줄에 기록한다. 관련 테스트 44개, DeliveryDispatch 2회, Bingo 1회와 Java aggregate 1회가 통과했다.
전체 core 테스트에는 아래 별도 실패 2개가 남아 있다.

증거 디렉터리는 `/tmp/java-delivery-teardown-evidence/`다. `main`에서 작업했으며
commit과 package 재빌드는 하지 않았다. 설치 JAR SHA256은
`66a0abe0a5fd44993143fc37fb223d83612554f454f7c3ca826ca146027dd6b7`이다.

## 원인과 소유 지점

두 진단 실행에서 업무 완료 표식은 모두 나왔지만 CourierSession은
`ForceStopped/TeardownFailed`였다. 첫 실행은 `stage=stream_close`와
`IllegalStateException: STREAM binding cleanup did not complete`를 기록했다.
두 번째 실행의 `delivery-cause-roles/courier-session.log:58`에는 같은 event에
`cause_type=systems.zlink.framework.errors.ZLinkFrameworkException`,
`cause_message=durable request target lifecycle ended`가 있다. 이 파일의 원인 event는
1회이고, 기존 `ZLINK_FRAMEWORK_TERMINATION` 표식도 1회다.

| 원인 위치 | 동작과 수정 |
|---|---|
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequest.java:57` | Logical Actor owner가 종료됐으면 `UNAVAILABLE`로 완료한다. 올바른 terminal이며 변경하지 않았다. |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:2230` | 원격 unbind의 성공에만 연결했던 local delivery 제거를 `whenComplete`에 연결했다. 실패해도 해당 session·Actor·generation·stream identity만 제거한다. |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java:374` | 원격 unbind terminal에서 socket의 해당 binding을 제거한다. 원래 예외는 호출자에게 전달한다. |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java:583` | Socket close는 남은 binding을 정리한다. 수정 전 실패한 unbind가 남아 재제출됐고, logical owner 종료 예외를 native 오류만 검사하는 분류기가 정상 cleanup으로 인식하지 못했다. 이 분류기와 close 경로는 변경하지 않았다. |

대안은 close의 오류 분류기에 Framework `UNAVAILABLE` 예외를 추가하는 것이었다.
그 방법은 이미 terminal인 binding을 보관하고 다시 제출하는 경로를 유지한다.
선택한 수정은 unbind terminal에서 local 정리를 완료하므로 새로운 예외 분류·재시도·
timeout·상태를 추가하지 않는다. 원격 실패를 성공으로 바꾸지도 않는다.

- 소유 계층: **Java Framework의 STREAM–Actor binding lifecycle**. `runtime/binding`은 Framework adapter이며 `bindings/java` package 구현이 아니다.
- Spec 조항: [host relocation flow 한국어 §14](../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md#14-shutdown과-relocate의-경쟁), [영어 §14](../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.en.md#14-the-race-between-shutdown-and-relocate)의 callback → local cleanup → transport 정리와 `Stopped/None`, callback/resource failure의 `ForceStopped/TeardownFailed`; [session–Actor binding §7](../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md#7-disconnect-통지)의 all-settled 통지와 실패·deadline 뒤 local cleanup.
- 교차언어 대조: .NET `ZLinkSessionActorBindingRegistry.cs:129–139`는 통지 결과와 관계없이 binding을 제거한다. Backend wrapper `ZLinkBackendStreamSocketWrapper.cs:314–335`는 session service 해제에 위임한다. Node `managed-stream.ts:342–355`는 native service가 remote tombstone 전 local delivery를 제거한다는 계약을 사용한다. Java가 Framework 안에서 구현한 remote unbind에서만 성공 조건이 남아 있던 구조적 차이다.
- 변경 분류: **B — 기존 Framework lifecycle 결함과 종료 원인 관찰 누락 수정**. Core·binding 결함 또는 spec gap으로 분류하지 않는다.
- 수정 전/후 규칙 수: **binding 정리 규칙 2 → 1**. 성공 때 제거하고 실패 때 close가 다시 제출하던 규칙을, 모든 unbind terminal에서 exact local binding을 정리하는 규칙으로 통합했다.

## 종료 원인 기록

`ZLinkFrameworkShutdown`은 기존 cleanup action에 소유 단계 이름을 붙이고 첫 실패를
예외로 전달한다. 추가 cleanup 실패는 기존 suppressed 정보로 보존하며 새 stack은
생성하지 않는다. `ZLinkFrameworkRuntime`은 내부 forced result에 이 예외를 전달해,
최종 상태 게시 한 곳에서 기존 `ZLinkRuntimeEventDispatcher`로 보낸다.
공개 termination result의 outcome/reason 계약은 그대로다.

종료 예외가 전달된 최종 상태 event의 필드는 `outcome`, `reason`, `stage`,
`exception_type`, `exception_message`, `cause_type`, `cause_message`다. Message의 CR/LF는
escape해 한 줄로 유지하며 Throwable을 logger에 넘겨 stack을 출력하지 않는다.
`stream_close`, `spot_close`, `instance_close`, `channel_close`, `context_close`와
나머지 기존 cleanup action의 단계가 포함된다. Spring의 기존 종료 표식과 중복 방지
판정은 그대로이며, 예외를 기록하는 logger를 추가하지 않았다.

관찰 대조로 .NET `ZLinkDrainCoordinator.cs:408–410,508–510`은 종료 예외를 logger에
전달한다. Java는 같은 원인 정보를 stack 대신 기존 최종 상태 event 한 줄에 담는다.
관찰 정보 때문에 공개 result field나 별도 실패 registry를 추가하는 대안은 선택하지 않았다.

## 검증 결과

JVM/Gradle 명령은 `TMPDIR=/dev/shm/zlink-tmp-java`, `ZLINK_LIBRARY_PATH` unset과
`flock -w7200 /tmp/zlink-jvm-gate.lock`을 사용했다. Sample은 추가로
`flock -w7200 /tmp/zlink-samples-gate.lock`을 획득한다. 앞선 gate가 해제할 때까지 기다렸다.

| 검증 | 결과 | 증거 |
|---|---|---|
| DeliveryDispatch 원인 진단 1/2 | exit 1, 업무 완료, host 5/6 Stopped/None, courier-session TeardownFailed | `delivery-diagnosis.log`, `delivery-diagnosis-roles/` |
| DeliveryDispatch 원인 진단 2/2 | exit 1, 업무 완료, host 5/6 Stopped/None, courier-session TeardownFailed; 실제 내부 원인 기록 | `delivery-cause.log`, `delivery-cause-roles/` |
| 신규 unbind 회귀 테스트, 수정 전 | 1개 실패, socket close가 같은 binding을 재제출 | `unbind-before.log`, `unbind-before.xml` |
| 관련 subsystem 테스트 | **44/44 PASS**: M6B 36, shutdown 관찰 2, host drain 6 | `owner-focused.log`, `owner-focused-results/` |
| 전체 core test + contractTest 1회 | **core 1238/1240 PASS, 2 FAIL**, contract **96/96 PASS** | `full-unit-contract.log`, `full-core-results/`, `full-contract-*/` |
| 전체 실패만 focused 재검증 | 2개 중 timer PASS, mesh admission FAIL | `unrelated-focused.log`, `unrelated-focused-results/` |
| 최종 DeliveryDispatch 1/2 | **exit 0, host 6/6 Stopped/None** | `delivery-final-1.*` |
| 최종 DeliveryDispatch 2/2 | **exit 0, host 6/6 Stopped/None** | `delivery-final-2.*` |
| 최종 Bingo | **exit 0, host 7/7 Stopped/None** | `bingo-final.*` |
| Java aggregate 1회 | **exit 0, Java sample 7/7 PASS** | `aggregate-final.log`, `aggregate-final-roles/`, `aggregate-zoneworld-roles/` |

전체 명령은 `./gradlew --no-daemon :zlink-framework-core:test contractTest --continue`를
1회 실행했다. Core 실패 뒤에도 계약 검증을 완료하기 위해 `--continue`를 사용했다.
Contract 결과는 core 27, Kotlin 17, testkit 48, provider abstractions 4다. 이 중 provider
4개는 `UP-TO-DATE` 결과를 재사용했으며 이번 명령에서 새로 실행된 contract는 92개다.
Kotlin source는 변경하지 않았다.

Aggregate는 `ZLINK_SAMPLE_LANGUAGES=java bash samples/run_samples.sh`를 1회 실행했다.
TicTacToe, Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat, ZoneWorld가
모두 통과했다. Aggregate의 DeliveryDispatch도 host 6/6, Bingo도 host 7/7이
`Stopped/None`이다. ZoneWorld 본 실행의 임시 역할 로그는 삭제 전에 열린 파일을
유지해 `aggregate-zoneworld-roles/`에 보존했다. 이 로그에는 시나리오의 의도적 crash와
재시작이 포함되며 `ForceStopped` 표식은 없다.

최종 `git diff --check`는 통과했고, 설치 JAR SHA256은 작업 시작과 종료 때 동일하다.
Core·Java binding·다른 언어·공유 sample·보호 문서는 수정하지 않았다.

요청에서 알려준 `descriptorFenceReplacesEndpointOnlyIntent`와
`ZLinkServiceOperationRegistryTest.cancellationAtomicallyTakesTheOperationBeforeClose`
(:109 assertion)은 **이번 전체 실행에서 모두 PASS**다. 대신 다음 실패를 보존했다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`,
  테스트 :649 → `awaitState` :1400: replacement peer의 `ADMITTED` 상태를 관찰하지 못했다.
  Focused 실행에서도 실패했다. 이 테스트는 raw mesh connection 교체를 직접 검증하며
  수정한 STREAM unbind와 host shutdown을 사용하지 않는다.
- `ZLinkSpotTimerRegistryTest.relocationAbortResumesTheExistingTimerHandle`, :361:
  resume 뒤 timer callback 대기 assertion이 실패했다. Focused 실행은 PASS다.
  테스트가 직접 만드는 timer registry와 실행 경로는 수정하지 않았다.

문서 독립 리뷰에서 runtime·회귀 테스트·보존 로그를 대조했다. 테스트 수 집계에서
누락했던 Gradle `__TEST-*` XML 13개 case를 반영했고, 예외 event 조건과 계약 링크를 보완했다.

## 변경 파일

- Java core runtime: `ZLinkJavaStreamSocket.java`, `ZLinkJavaRawSpotNode.java`,
  `ZLinkFrameworkRuntime.java`, `ZLinkFrameworkShutdown.java`, `ZLinkRuntimeEventDispatcher.java`.
- Java core test: `ZLinkJavaRawSpotNodeM6BTest.java`, `ZLinkFrameworkShutdownTest.java`.
- 이 작업 결과 문서.

## BLOCKERS

- 전체 core gate에는 위 mesh admission 실패와 전체 실행에서 발생한 timer 실패가 남는다.
  관련 expectation을 바꾸거나 unrelated 구현을 수정하지 않았다.
