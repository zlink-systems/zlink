# Kotlin DeliveryDispatch CourierSession 종료 수정 결과

2026-09-05. Kotlin도 shared Java Framework의 STREAM–Actor binding 경로를 사용한다.
`ZLinkBoundActor`가 disconnect 통지의 동기 제출 예외를 cleanup stage에 연결하지 않아,
해당 Actor의 unbind를 생략하는 결함을 수정했다. 기존 stage 변환기를 사용해 통지의
동기·비동기 실패가 같은 unbind와 terminal 완료 처리를 거치게 한다. Kotlin DeliveryDispatch
단독 2회와 Java/Kotlin 전체 sample 14개가 통과했다. 전체 core test에는 기존 mesh
admission 실패 1개가 남는다.

증거 디렉터리는 `/tmp/kotlin-delivery-teardown-evidence/`다. `main`에서 작업했으며,
commit과 binding package 재빌드는 하지 않았다. 작업 시작·완료 때 동일한 JAR SHA256은
`bf3ad3df87607046dc18fda65c65e81e0d4f2e2a991d877f5f1202f73ebc454c`다.

## 원인과 소유 지점

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkBoundActor.java:502`:
  수정 전에는 `notifyRemoteDisconnected()` 또는 managed Actor 통지가 동기 예외를 던지면
  아래 unbind chain을 만들지 못했다. 이미 만든 `disconnect` future도 terminal이 되지 않았다.
- 같은 파일 `:520`: 통지의 terminal 뒤 backend unbind를 제출하는 기존 소유 지점이다.
  기존 Java 수정은 이 제출에 도달한 unbind의 성공·실패를 정리하지만, 제출 자체가
  생략된 binding까지 정리할 수는 없다.
- `ZLinkSessionActorsRuntime.java:379`: all-settled 통지는 동기 예외를 실패 future로 바꾸고
  runtime의 binding 목록을 제거한다. 수정 전에는 socket에 남은 binding과 이 목록이 달라졌다.
- `ZLinkJavaStreamSocket.java:583`: close가 남은 socket binding을 정리하다가 원격 owner의
  lifecycle 종료 또는 admission deadline을 관찰하면 `STREAM binding cleanup did not complete`를
  전달한다. 오류 분류기에 예외를 추가하는 방식은 선택하지 않았다.

원본 Kotlin 실행 1은 exit 0, host 6/6 `Stopped/None`이었다. 실행 2는 업무 완료 뒤
CourierSession만 `ForceStopped/TeardownFailed`였다. `repro-2-roles/courier-session.log:186`의
종료 원인 event는 다음 정보를 포함한다.

```text
stage=stream_close
exception_type=java.lang.IllegalStateException
exception_message=STREAM binding cleanup did not complete
cause_type=systems.zlink.framework.errors.ZLinkFrameworkException
cause_message=durable request was not admitted before its deadline
```

제공된 gate9a 로그에는 같은 stage와 exception에
`cause_message=durable request target lifecycle ended`가 있다. 두 경우 모두 상세 event와
`ZLINK_FRAMEWORK_TERMINATION` marker가 각각 한 번 기록된다. Kotlin이 shared reporting을
우회하지 않는다. 종료 로그 자체는 앞선 통지 제출 예외를 기록하지 않으므로, 누락 경로는
아래 회귀 테스트로 별도로 검증했다.

## 언어 경로와 수정

Kotlin `CourierSession.kt:32`는 `ZLinkSuspendingSession`의 callback을 통해 bound Actor를
순차적으로 기다린다. Java `CourierSession.java:44`는 `CompletableFuture.allOf`로 통지를
모은다. 그러나 양쪽 모두 `ZLinkStreamRuntime.java:1564`가 application callback 전에
`ZLinkSessionActorsRuntime.notifyDisconnectedAll`을 실행한다. Kotlin module의
`ZLinkSuspendingHandlers.kt:317`은 coroutine을 CompletionStage로 연결하고,
`ZLinkCoroutineTurnAwait.kt:15`는 그 stage를 기다리며 별도 binding 상태를 소유하지 않는다.

따라서 sample이나 Kotlin coroutine wrapper에 cleanup을 추가하지 않고 shared owner의
`ZLinkHandlerStages.fromStageSupplier`를 재사용했다. 원래 통지 예외는 기존 결과 future에
전달하고, 기존 exact binding 제거와 중복 통지 방지를 유지한다. Close의 오류 분류기를
확장하는 대안은 누락된 unbind와 미완료 disconnect future를 남기므로 선택하지 않았다.

- 소유 계층: **shared Java/Kotlin Framework의 Session–Actor disconnect lifecycle**, `runtime/actors/ZLinkBoundActor`.
- Spec 조항: [host relocation flow 한국어 §14](../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md#14-shutdown과-relocate의-경쟁), [영어 §14](../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.en.md#14-the-race-between-shutdown-and-relocate)의 accepted callback 완료 후 transport cleanup; [session–Actor binding §7](../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md#7-disconnect-통지)의 all-settled 통지와 failure 뒤 cleanup.
- 교차언어 대조: Java와 Kotlin은 같은 owner를 사용하며 coroutine wrapper에는 별도 binding registry가 없다. .NET `ZLinkSessionActorBindingRegistry.cs:110–139`는 각 통지 실패를 관찰한 뒤 모든 exact binding을 제거한다. Kotlin 전용 구현 결함으로 분류하지 않는다.
- 변경 분류: **B — 기존 Framework disconnect 제출 실패 처리 결함**.
- 수정 전/후 규칙 수: **통지 실패 처리 규칙 2 → 1**. 동기 실패의 cleanup 생략과 비동기 실패의 cleanup을, 모든 통지 terminal 뒤 기존 unbind를 실행하는 규칙으로 통합한다.

## 검증 결과

JVM/Gradle은 `TMPDIR=/dev/shm/zlink-tmp-java`, `ZLINK_LIBRARY_PATH` unset,
`flock -w7200 /tmp/zlink-jvm-gate.lock`을 사용한다. Sample은
`flock -w7200 /tmp/zlink-samples-gate.lock`도 획득한다. DeliveryDispatch 각 실행의 역할
로그와 기존 flow 로그를 별도 디렉터리에 보존한다.

| 검증 | 결과 | 증거 |
|---|---|---|
| 원본 Kotlin DeliveryDispatch 1/2 | exit 0, host 6/6 Stopped/None | `repro-1.*`, `repro-1-roles/` |
| 원본 Kotlin DeliveryDispatch 2/2 | exit 1, CourierSession stream_close 실패 | `repro-2.*`, `repro-2-roles/` |
| 신규 회귀 테스트, 수정 전 | FAIL: actor-1 unbind 누락, actor-2만 unbind | `notification-before.log`, `notification-before.xml` |
| 관련 subsystem 테스트 | **60/60 PASS**: session binding 17, bound Actor 7, raw Spot M6B 36 | `owner-focused.log`, `owner-focused-results/` |
| 수정 후 Kotlin DeliveryDispatch 1/2 | **exit 0, host 6/6 Stopped/None** | `delivery-final-1.*`, `delivery-final-1-roles/` |
| 수정 후 Kotlin DeliveryDispatch 2/2 | **exit 0, host 6/6 Stopped/None** | `delivery-final-2.*`, `delivery-final-2-roles/` |
| Java + Kotlin 전체 sample 1회 | **exit 0, Java 7/7 + Kotlin 7/7 PASS** | `aggregate-final.log`, `aggregate-java/`, `aggregate-kotlin/` |
| core test + Kotlin test + contractTest 1회 | **core 1244/1245 PASS, Kotlin 67/67 PASS, contract 96/96 PASS**, exit 1 | `full-unit-contract.log`, `full-core-results/`, `full-kotlin-results/`, `full-contract-*/` |

신규 회귀 테스트 `ZLinkSessionActorBindingContractTest.java:174`는 첫 Actor의 통지 제출이
동기 `ZlinkSubmitException(NOT_CONNECTED)`로 실패해도 모든 Actor의 unbind가 한 번씩
실행되는지 확인한다. 원래 예외 전달, runtime binding 제거, disconnect future 완료와
같은 Actor의 재통지 시 통지 제출·unbind 재실행 방지도 검증한다.

전체 명령은 `./gradlew --no-daemon :zlink-framework-core:test :zlink-framework-kotlin:test contractTest --continue`를
한 번 실행했다. Core 실패 뒤에도 Kotlin과 contract 결과를 확보하기 위해 `--continue`를 사용했다.
Contract는 core 27, Kotlin 17, provider abstractions 4, testkit 48개가 모두 통과했다.
Core 실패는 `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`,
`:649 → awaitState :1400`의 `peer state was not observed: ADMITTED`다. 기존
[Java 종료 수정 결과](./fix-java-deliverydispatch-courier-session-teardown-summary.md#blockers)에
기록된 같은 mesh admission 실패이며, 해당 구현과 expectation은 수정하지 않았다.

Aggregate 명령은 `bash samples/run_samples.sh`를 Java/Kotlin 기본 선택으로 한 번 실행했다.
TicTacToe, Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat, ZoneWorld가
두 언어에서 모두 통과했다. Aggregate의 Java·Kotlin DeliveryDispatch도 각각 host 6/6이
`Stopped/None`이다. 단독 Kotlin 2회에서는 각 host의 READY와 종료 marker가 한 번씩이며,
`FORCE_STOPPED`가 없음을 별도로 확인했다.

`git diff --check`와 문서의 상대 link 대상 검증은 통과했다. 문서의 코드·증거 부합과
작성 원칙을 독립 리뷰했으며, 회귀 테스트 설명을 실제 검증한 통지 제출 횟수에 맞췄다.
Core·binding package, 다른 언어 구현, sample, 공유 sample과 보호 문서는 수정하지 않았다.

## 변경 파일

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkBoundActor.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorBindingContractTest.java`
- 이 결과 문서.

## BLOCKERS

- 전체 core gate에는 위의 기존 mesh admission 실패 1개가 남는다.
