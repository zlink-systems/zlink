# Java durable replay 2차 결과

Stage 1 진단 뒤 A/B 범위를 구현했다. Focused 34건, 전체 core 1,221건과 TicTacToe·GameQuest
각 1회가 모두 통과했다. commit과 Core·binding 재빌드는 하지 않았다.

- Owner: Java raw mesh의 durable sender(`ZLinkJavaDurableRequest`)가 encoded identity·deadline·typed admission 이력을 소유한다. Core는 handover 완료, binding은 submit/request 결과 생성과 전달을 소유한다.
- Spec: actor-model §9 sender replay bullets(:668–680), Core socket README §4(:160–169, bb730c654f). 동일 OperationId/header, terminal envelope 전 replay, 전체 remaining deadline, never-admitted=Unavailable/admitted=DeadlineExceeded.
- Parity: C++ raw_route_port.hpp:51–70의 phase 보존과 raw_mesh_node_owner.cpp의 request_parts 보유, Node service-stateful-runtime.ts의 header 고정·admission 이력 소비에 맞췄다. Java에서만 필요했던 전달 경계 수정은 typed 예외를 coarse result로 축소하던 구조적 차이 때문이다.
- Class: A(갱신된 durable sender 계약 적응) + B(Java typed phase 소실·bind identity 재생성 결함 수정).

## Diff

아래 경로는 `framework/languages/java/zlink-framework-core/` 기준이다.

| 변경 파일 | 결과 |
| --- | --- |
| `src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequest.java` | durable request의 첫 encoded frame을 고정한다. 원래 typed submit/request 예외로 admission 이력을 구분하고, NOT_CONNECTED·TIMED_OUT에 remaining deadline으로 replay한다. 소진 오류의 cause도 유지한다. |
| `src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java` | Actor Join/create와 bound bind를 durable sender에 연결했다. preflight가 synthetic request 예외를 만들지 않는다. create는 별도 registry timeout 경쟁을 제거하고 transport 완료 후 terminal을 decode한다. 공용 requestResult와 application request 호출자는 그대로 유지한다. |
| `src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotJoinCall.java` | admission 전 gate 소진은 Unavailable이다. typed request timeout을 topology로 다시 분류하던 코드를 제거했다. |
| `src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorCreationCoordinator.java` | target 선택·ready 대기에서 제출 전에 소진되면 Unavailable이다. |
| `src/main/java/systems/zlink/framework/runtime/actors/ZLinkBoundSessionRuntime.java` | bind를 한 번만 submit해 generation이 재할당되지 않는다. 고정 2초 attempt budget을 제거하고 호출의 전체 timeout을 전달한다. route 대기 소진은 Unavailable이다. |
| `src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequestTest.java` | operation별 3행 행렬과 preflight, admission 이력 유지, synchronous typed failure, terminal/decode 뒤 중단, permanent failure 보존을 검증한다. |
| `src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeDurableReplayTest.java` | 실제 native context/raw mesh에서 세 operation의 route 부재가 deadline까지 대기한 뒤 Unavailable로 끝나는지 검증한다. |
| `src/test/java/systems/zlink/framework/runtime/actors/ZLinkActorRetrySchedulerTest.java` | bound route 미수락 소진의 spec 기대를 Unavailable로 교정했다. bind 1회 submit과 전체 3초 budget 전달을 추가 검증한다. |

진단 문서는 `doc/plan/c016-worklog/stage2-java-durable-replay-2-diagnosis.md`에 먼저 작성했다.
새 public API, errno 분류, deadline 연장, sample 변경은 없다. 기존 작업과 다른 언어 파일은
수정하지 않았다. `git diff --check`가 통과했다.

## 회귀 행렬

| 상황 | Actor Join | Actor create | Bound-session bind |
| --- | --- | --- | --- |
| 전체 deadline 동안 미수락 | PASS — typed Submit NOT_CONNECTED → Unavailable | PASS — 동일 | PASS — 동일 |
| admission 후 reply 유실 | PASS — typed Request TIMED_OUT → DeadlineExceeded | PASS — 동일 | PASS — 동일 |
| handover-stranded NOT_CONNECTED 후 성공 | PASS — 동일 header, 전체 remaining budget, 2 attempts | PASS — 동일 OperationId/header | PASS — 동일 binding generation/header |

위 9건은 각 operation의 실제 wire codec으로 만든 header와 scripted typed binding completion을
사용하는 sender 회귀다. reply 유실 행은 attempt에 전체 remaining deadline을 주므로 1 attempt로
소진된다. handover 행은 첫 request가 즉시 NOT_CONNECTED로 끝나고 다음 request가 terminal을
반환한다. 세 native raw mesh route 부재 회귀는 별도로 통과했다.

이 행렬은 실제 네트워크 handover 및 target의 durable terminal store 실행 횟수를 증명하는
integration test는 아니다. Core의 즉시 NOT_CONNECTED 완료 구현은 별도 작업이며, 이 작업에서는
그 결과를 sender 입력으로 검증했다.

## Gate counts

| 검증 | 실행 횟수 | 결과 |
| --- | --- | --- |
| `:zlink-framework-core:compileJava` | 1 | PASS |
| Focused test classes | 1 | 6개 클래스, 34 tests, 0 failures/errors/skips |
| `:zlink-framework-core:test` 전체 | 1 | 203개 클래스, 1,221 tests, 0 failures/errors/skips |
| TicTacToe | 1 | exit 0, `tictactoe-placement=completed` |
| GameQuest | 1 | exit 0, `gamequest-server-evidence=completed`, `gamequest=completed`, `gamequest-placement=completed` |

Focused 대상은 위 touched test 3개와 기존 `ZLinkJavaRawMeshNodeCanonicalActorJoinTest`,
`ZLinkActorCreationCoordinatorTargetSelectionTest`, `ZLinkActorSpotJoinCallTest`다. 알려진 C 분류
`ZLinkJavaRawMeshNodeM6ATest` 실패 2건은 재현되지 않았다(해당 클래스 28/28 PASS).
GameQuest log의 `Killed` 한 줄은 runner :326의 owner 장애 시나리오가 수행하는 `kill -9` 결과이며
runner 자체는 성공했다.

모든 실제 Gradle 실행은 `TMPDIR=/dev/shm/zlink-tmp-java`,
`flock -w7200 /tmp/zlink-jvm-gate.lock` 아래 실행했다. Sample은 runner 전체에 같은 lock을 적용했다.
09:48:46에 생성된 기존 `systems.zlink:zlink:0.17.0` jar를 사용했고 binding package를 재생성하지
않았다(사용자 제공 Java 수정 기준 c9d294c44f). `ZLINK_LIBRARY_PATH`는 설정하지 않았다.

보존한 로그와 JUnit XML:

- `/dev/shm/zlink-tmp-java/durable-replay-2/{compile,focused,core,tictactoe,gamequest}.log`
- `/dev/shm/zlink-tmp-java/durable-replay-2/{focused-results,core-results}/`
- GameQuest run: `/dev/shm/zlink-tmp-java/tmp.k80rZy1JjN/` — 역할별 file log 보존.

## BLOCKERS

Java 구현과 지정된 gate의 blocker 및 남은 실패는 없다. 실제 Core handover 즉시 완료에 대한
native integration 검증은 별도 Core 작업의 범위로 남는다. 이번 결과를 그 Core 변경의 검증으로
사용해서는 안 된다.
