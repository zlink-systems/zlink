# Stage 2 Java durable replay 결과

승인 진단에 누락된 admission 정보 소실과 bind identity 생성 경계를 확인하여 구현 전에
중단했다. 작업 지시의 “if the diagnosis turns out incomplete, STOP and report”를 적용했다.
아래 내용은 코드 조사 결과이며 실행으로 검증한 회귀 결과가 아니다.

- Owner: Java Framework의 durable Actor lifecycle·bound-session sender. Core·binding의 admission 분류는 소비만 한다.
- Spec: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-680` — 동일 OperationId replay, 전체 remaining deadline, admission 이력에 따른 exhaustion kind, application request 제외.
- Parity: C++ `framework/src/runtime/backend/raw_route_port.hpp:51-70`은 submit/request typed result를 별도로 보존한다. Java raw mesh의 아래 변환은 이 구분을 소실한다. 동일 replay 정책 외에 Java 내부 전달 경계의 보완이 필요하다.
- Class: 승인 범위는 A(계약 적응). 구현 변경은 없으며, 추가 발견의 분류·수정 경계는 감독자의 진단 보완이 필요하다.

## Diff

이 결과 문서만 추가했다. Java runtime·test·sample 변경, Core·binding 재빌드, commit은 없다.
작업 시작 branch는 `main`이며 기존 .NET·Node 변경과 untracked 파일은 보존했다.

## BLOCKERS

아래 Java 파일은
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/`
기준이다.

### Binding typed result 전달 경계

`binding/ZLinkJavaRawServicePort.java:173-184`는 binding 예외를 보존하지만, 그 위 raw mesh에서
예외의 phase와 종류를 잃는다. 승인 진단은 port의 보존을 확인한 뒤 상위 sender가 그 정보를
소비하도록 제안했으나 다음 변환을 수정 경계로 명시하지 않았다.

- `binding/ZLinkJavaRawMeshNode.java:3612-3623`의 `requestResult()`는
  `ZlinkSubmitException`을 처리하지 않아 `SubmitResult.NOT_CONNECTED`도
  `RequestResult.INTERNAL_ERROR`가 된다.
- Actor create는 `:3489-3498`에서 이 변환을 호출하고, `:3854-3858`에서 모든 non-OK 결과를
  원인 없는 `IllegalStateException`으로 바꾼다. 따라서 coordinator가 binding의 typed
  admission marker를 읽을 수 없다. 진단 행렬의 admitted timeout 설명도 보완해야 한다:
  이 callback이 먼저 완료하는 경로에서는 typed `TIMED_OUT`조차 위 예외로 전달된다.
- Bound bind는 `:2910-2920`에서 같은 변환 뒤 새 `ZlinkRequestException`을 만든다.
  실제 submit 실패가 request completion처럼 보이므로 이 예외 타입을 그대로
  `admittedOnce=true`의 근거로 삼을 수 없다.
- Bound bind의 preflight `:2873-2880`도 요청을 제출하지 않고
  `ZlinkRequestException(RequestResult.NOT_CONNECTED)`을 만든다. 이는 실제 binding이
  반환한 request terminal이 아니다.

D-B97의 typed result만 소비하려면 위 Framework 변환 이전에 실제 binding 결과를
보존·소비해야 한다. 공용 `requestResult()`는 요청 범위 밖 호출도 사용하므로 전역 변경을
임의로 적용하지 않았다. 감독자는 durable 경로에서의 phase 보존 위치와 preflight 처리까지
최소 수정에 포함해 진단을 보완해야 한다.

### Bound bind의 replay 소유 위치

- `actors/ZLinkBoundSessionRuntime.java:168-173`은 retry마다 `stream.bindActor(...).submit(...)`을 호출한다.
- `binding/ZLinkJavaStreamSocket.java:337-355`는 `submit` lambda 안에서 새 binding generation을
  할당한다. `bindActor()` 반환 객체만 재사용해도 generation은 재할당된다.
- `binding/ZLinkJavaRawSpotNode.java:2187-2193`을 거쳐
  `binding/ZLinkJavaRawMeshNode.java:2889-2901`에서 새 correlation과 encoded header를 만든다.

승인 진단은 `bindRelayUntilAccepted()`를 retry owner로 지목했지만, 이 경계에서 재호출하면
동일 encoded request를 유지할 수 없다. Predicate와 2초 timeout만 바꾸면 handover timeout
후에도 generation·correlation이 바뀐다. Identity를 생성하는 내부 경계에서 replay하도록
소유 위치를 확정하거나, 기존 내부 operation의 생성·submit 경계를 조정하는 진단이 필요하다.
이 선택을 임의로 구현하지 않았다.

## 회귀 행렬

| Operation | 전체 deadline 동안 route 부재 | Admission 후 reply 없음 | Handover 1회 timeout 후 성공 |
| --- | --- | --- | --- |
| Actor Join | 미실행; 요구값 Unavailable | 미실행; 요구값 DeadlineExceeded | 미실행; 동일 identity replay 필요 |
| Actor create | 미실행; typed submit 전달 경계 보완 필요 | 미실행; typed timeout 전달 경계 보완 필요 | 미실행; 동일 identity replay 필요 |
| Bound-session bind | 미실행; synthetic request 예외와 실제 admission 구분 필요 | 미실행; typed request 전달 경계 보완 필요 | 미실행; generation·correlation 보존 위치 확정 필요 |

## Gate counts

| 검증 | 실행 횟수 | 결과 |
| --- | --- | --- |
| Touched test classes | 0 | Test 변경 없이 중단 |
| `:zlink-framework-core:test` | 0 | 진단 불완전으로 구현 전 중단 |
| TicTacToe sample | 0 | 동일 사유 |
| GameQuest sample | 0 | 동일 사유 |

새로 실행한 test 실패는 없다. 알려진 C 분류 `ZLinkJavaRawMeshNodeM6ATest` 2건도 재확인하지
않았다. Gradle을 호출하지 않아 JVM gate lock 획득이나 local package 재빌드는 수행하지 않았다.
