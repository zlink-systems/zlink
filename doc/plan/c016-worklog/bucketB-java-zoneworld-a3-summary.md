# Bucket B — Java ZoneWorld `ZW-A3` timeout 조사 결과

## 결론

`ZW-A3`의 timeout은 zone relocation이나 `MoveMsg` reply 유실이 아니다. 실패 stack의
`ScenarioSupport.java:142`는 `(25,25) → (49,49)`의 `zone-nw` 내부 이동에서
`ZoneStateNotify`를 기다리는 자리다. `MoveMsg`는 one-way이고, 이 구간에는 zone 경계가 없다.

원인은 gateway가 zone node의 command 36 bound-session push를 처리할 때
`sendBoundSessionPushAsync()`가 이름과 달리 호출 thread에서 STREAM socket state lane에 진입하고
`join()`한 데 있다. state lane이 앞선 작업으로 바쁘면 command 36 infrastructure 처리도 함께
멈추므로, 해당 session의 `ZoneStateNotify`가 client STREAM에 admission되지 못할 수 있었다. 이는
이전 A5에서 control frame에 대해 제거한 synchronous state-lane wait와 같은 계열이지만,
이번에는 bound-session data push 경로에 남아 있던 경우다.

수정은 bound-session frame을 소유 복사한 뒤 timeout 조회와 native send를 socket state lane에
queueing하고 즉시 flatten된 stage를 반환하도록 했다. timeout과 assertion은 바꾸지 않았다.
수정 후 final gate와 같은 prefix를 5회, selector 없는 FULL을 2회 실행했고 모두 통과했다.

## 실패 지점 판정

최종 gate log는 다음 순서를 보인다.

- `ZW-G4, ZW-B8, ZW-G1, ZW-G2-rid, ZW-G5, ZW-A1, ZW-A2` 통과
- `Scenarios.a3(Scenarios.java:77)`의 `player.moveTo(49, 49)` 실행
- `ScenarioSupport$Game.moveTo(ScenarioSupport.java:142)`에서 `TimeoutException`
- `scenario ZW-A3 failed`, 최종 marker withheld

`moveTo()`는 먼저 다음 좌표의 old/new zone을 비교한다. zone이 다르면
`ZoneChangedNotify`를 기다리지만, 같으면 목표 좌표가 포함된 `ZoneStateNotify`를 기다린 뒤
`MoveMsg`를 보낸다(`ScenarioSupport.java:118-144`). A3는 두 reject 뒤 `(25,25)`에서 시작하여
x축을 `30,35,40,45,49`, 이어 y축을 `30,35,40,45,49`로 옮긴다. 열 단계 모두
`zone-nw → zone-nw`다. 따라서 line 142 timeout은 이 열 단계 중 한 `ZoneStateNotify` 대기이며,
zone pair나 handover가 없다.

server의 same-zone move는 actor 위치를 갱신하고 `UpdatePositionMsg`를 같은 spot에 보낸 뒤
`actor.send(new ZoneStateNotify(...))`를 반환한다(`ZoneSpot.java:186-205`). `MoveMsg` 자체는
one-way이므로 기다릴 reply가 없다.

원래 실패 run directory `/dev/shm/zlink-tmp-java/tmp.Sqt8VtelzP`는 조사 시점에 이미 없어 role
log를 회수할 수 없었다. client log에도 각 step 좌표가 없으므로, 열 좌표 중 정확히 어느
`ZoneStateNotify`가 마지막이었는지는 사후 확정할 수 없다. 실패가 30초 동안 다음 step으로
진행하지 않았고 주기 state push도 같은 predicate를 만족시킬 수 있다는 점은, 한 packet 생성
누락보다 해당 session의 bound push 진행이 막힌 현상과 일치한다.

## message-flow와 원인

기존 message-flow `NORMAL`, `ZLINK_JAVA_STREAM_TRACE=1`, run-dir 보존을 사용했다. 임시 logging은
추가하지 않았다. 수정 전 final-prefix 재현은 유효한 5회에서 모두 통과해 원래 runtime 실패
trace를 새로 얻지는 못했다. 대신 같은 blocking 경계를 고정하는 regression test를 먼저 red로
확인했다.

통과 run의 A3 actor `a3-8441d1`은 zone-node-2의 `zone-nw`에 있었다. 최종 `(49,49)`에 해당하는
12번째 A3 `MoveMsg` flow `01a06e38-3191-72b4-812d-22202728fbf7`의 timeline은 다음과 같다. 앞의
두 flow가 reject이고 뒤 열 flow가 `moveTo(49,49)`다.

| 시각 | role과 transition | 관찰 |
|---|---|---|
| 05:59:31.601~.603 | gateway, STREAM `received → admitted → dispatched → actor sent → completed` | `MoveMsg`, actor `a3-8441d1`, sequence 20 |
| 05:59:31.603~.606 | zone-node-2, command 24 enqueue와 actor `received → admitted → dispatched → completed` | owner `zone-nw`; 같은 flow로 `UpdatePositionMsg` 전송 |
| 05:59:31.606 | zone-node-2 | `send bound session accepted` |
| 05:59:31.607~.608 | gateway | command 36 수신 뒤 `bound session receive accepted` |

정상 flow는 `MoveMsg`가 actor에서 처리된 뒤 `ZoneStateNotify`가 command 36으로 gateway에 돌아오는
경로를 확인한다. 실패 stack과 코드 경로를 합치면 멈출 수 있던 경계는 다음 하나다.

1. gateway `ZLinkJavaRawMeshNode.dispatchBoundSessionSend()`가 command 36 handler가 반환한 stage를
   기다리지 않고 관찰한다(`ZLinkJavaRawMeshNode.java:6343-6387`).
2. handler는 `ZLinkSessionActorsRuntime.acceptBoundSessionSendAsync()`에서 현재 binding을 확인하고
   `deliverCurrentBoundSessionSendAsync()`를 호출한다(`ZLinkSessionActorsRuntime.java:1193-1217,
   1346-1357`).
3. 수정 전 `ZLinkJavaStreamSocket.sendBoundSessionPushAsync(rid, parts)`는 먼저
   `admissionTimeout()`을 호출했다. 이는 `nativeSocket()`을 거쳐
   `inStateLane(...).toCompletableFuture().join()`한다
   (`ZLinkJavaSocketBacked.java:11-15`, `ZLinkJavaStreamSocket.java:102-117`).
4. 따라서 handler가 stage를 반환하기도 전에 command 36 처리 thread가 socket state lane을
   기다렸다. socket lane이 밀린 동안 같은 gateway의 bound-session push 진행이 정지했다.

`asyncBoundSessionPushReturnsBeforeTheSocketStateLaneCanStartAdmission` test는 socket state lane을
선행 작업으로 붙들고 다른 thread에서 bound-session async push를 호출한다. 수정 전 코드를
대상으로 실행하면 호출이 1초 안에 stage를 반환하지 못해 의도한 assertion에서 실패했다.
이는 간헐 sample을 기다리지 않고 위 synchronous wait를 결정적으로 재현한다.

관련 계약은 다음 두 곳이 소유한다.

- `01-execution/02-handler-turn-and-execution-gate.ko.md:15-29,466-482`: binding completion 등
  infrastructure 작업은 application 대기와 무관하게 진행해야 한다.
- `00-foundation/04-interaction-model.ko.md:420-433`: actor-to-client message는 현재 binding의
  session FIFO를 사용하며, bound-session send는 async-only one-way admission 결과를 반환한다.

호출 thread에서 state lane을 기다리는 기존 구현은 async-only 반환과 infrastructure 진행 경계를
지키지 못했다.

## 수정과 회귀 방지

- `ZLinkJavaStreamSocket.java:228-255`
  - no-timeout bound-session push가 호출 thread에서 `admissionTimeout()`을 읽지 않는다.
  - caller가 곧 닫는 decoded frame을 `Message.from()`으로 소유 복사한다.
- `ZLinkJavaStreamSocket.java:293-335`
  - generic async STREAM frame과 bound-session frame이 `submitOwnedStreamFrameAsync()`를 공유한다.
  - timeout 조회와 `FrameworkStreamOperations.send()`를 socket state lane에 queueing하고 호출자에게
    즉시 합성 stage를 반환한다.
  - native operation 생성 뒤 소유 frame을 닫으며, 기존 설정 timeout과 기본 1초를 유지한다.
- `ZLinkJavaStreamSocketAsyncTerminalTest.java:109-174`
  - bound-session async 호출이 socket lane 차례가 오기 전에 stage를 반환해야 한다는 regression
    test를 추가했다.

검토한 다른 선택은 command 36 handler 쪽에서 별도 executor로 감싸는 방법과 STREAM socket
serialization을 우회하는 방법이다. 전자는 책임 모듈 밖에서 blocking 구현을 숨기고 다른
bound-session 호출자에는 문제가 남는다. 후자는 socket의 단일 state lane 순서를 깨뜨린다.
기존 async STREAM submit 경로를 bound-session push에도 공통 적용하는 수정이 public 경로와
session FIFO를 유지하면서 원인을 제거한다.

## 검증 결과

모든 Gradle/sample invocation은 `flock -w7200 /tmp/zlink-jvm-gate.lock` 안에서 실행했다. sample은
`TMPDIR=/dev/shm/zlink-tmp-java`, message-flow와 stream trace, run-dir 보존을 사용했다. 개별 runner의
native library 자동 설정이 없어 검증에는 file-valued
`ZLINK_LIBRARY_PATH=.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so`를 지정했다. 로그와 role
log 사본은 `zlink-work/c016/logs/bucketB-java-zoneworld-a3-measurement/`에 보존했다.

| 검증 | 결과 | 근거 |
|---|---|---|
| 수정 전 고정 regression test | 의도한 assertion에서 실패 | `red-focused-test-2.log`, `BUILD FAILED` |
| 수정 후 관련 test class | 통과 | `green-focused-test.log`, `BUILD SUCCESSFUL` |
| final-prefix 1~5 | 5/5 `ZW-A3` 포함 전부 통과, exit 0 | main run dir `tmp.oZdsd2Jhq1`, `tmp.X8u1YLOFG5`, `tmp.vYSYOKjBSj`, `tmp.lNzizl5wV7`, `tmp.9tuXY8x1YF` |
| FULL 1 | 전체 통과, `zoneworld=completed`, exit 0 | main `/dev/shm/zlink-tmp-java/tmp.Q6L9gJD2pG` |
| FULL 2 | 전체 통과, `zoneworld=completed`, exit 0 | main `/dev/shm/zlink-tmp-java/tmp.JrLvhrhkyx` |
| Java core 전체 test | 1,216개 중 기존 M6A 2개만 실패 | `core-test.log` |

수정 전 sample 측정의 final-prefix 1~5도 통과했다. 6회차는 sample 시작 전에
`UnsatisfiedLinkError: no zlink in java.library.path`가 발생해 A3 측정에서 제외했다. 이는 개별
runner가 local native library 경로를 설정하지 않은 환경 문제이며, 이후 실행에서는 위의
file-valued 경로를 명시했다.

Java core 전체 test의 남은 실패는 이번 경로와 무관한 기존 descriptor-fence test다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`

## 변경 파일

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocketAsyncTerminalTest.java`
- `doc/plan/c016-worklog/bucketB-java-zoneworld-a3-summary.md`

## BLOCKERS

- 기능 blocker는 없다. final-prefix 5회와 FULL 2회가 모두 통과했다.
- 원래 실패 role log가 소실되어 열 same-zone 좌표 중 정확히 어느 좌표의 notify가 마지막이었는지는
  확정할 수 없다. 실패 packet 종류와 zone 경계 없음은 stack line과 코드에서 확정했다.
- 요청에 적힌 `framework/languages/java/AGENTS.md`는 repository에 존재하지 않아 root와
  `framework/AGENTS.md` 규칙을 적용했다.
- 다른 작업의 Kotlin GameQuest 변경, 다른 언어, `core/**`, `bindings/**`, 보호된 spec/sample
  문서는 수정하지 않았고 commit하지 않았다.
