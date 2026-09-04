# Bucket B — Java ZoneWorld `ZW-A5` 3차 조사 결과

## 결론

`ZW-A5` timeout과 뒤이은 heartbeat 연쇄 실패는 gateway의 STREAM 수신 owner가 control frame의
물리 admission을 동기 대기해서 발생했다. `zlink-stream-recv`는 heartbeat pong 또는
`session-closing`을 보내면서 STREAM socket state lane에 작업을 넣고 `join()`했다. 같은 lane에서
앞선 bound-session push와 native send가 처리되는 동안 수신 owner는 다음 client frame, heartbeat와
disconnect를 읽지 못했다. native send 실패도 수신 frame 처리 예외로 전파되어 정상 peer를
`malformed STREAM frame`으로 격리했다.

수정은 heartbeat ping/pong과 `session-closing`을 기존 backend `sendAsync()` 경로에 제출하고,
Java STREAM의 async 구현이 socket state lane의 차례를 기다리지 않고 즉시 stage를 반환하도록
했다. 설정된 admission timeout 값과 socket serialization은 유지한다. server drain은 반환된
stage를 모아 control frame의 admission이 끝난 뒤 완료된다.

수정 후 최소 재현 selector `ZW-A4,ZW-A5`가 3회 연속 통과했다. selector 없는 FULL 실행도
2회 모두 `ZW-A5` 이후 전체 시나리오와 현행 Java runner의 최종 marker
`zoneworld=completed`까지 통과하고 exit 0으로 끝났다.

## 재현과 측정

기존 message-flow `NORMAL`, `ZLINK_JAVA_STREAM_TRACE=1`, run-dir 보존을 첫 재현부터 사용했다.
임시 logging은 추가하지 않았다. 실패 role log와 thread dump는
`zlink-work/c016/logs/bucketB-java-zoneworld-3-measurement/`에 보존했다.

| 실행 | selector | 결과 | 근거 |
|---|---|---|---|
| 재현 1 | `ZW-A4,ZW-A5` | A4 통과, A5의 두 join 뒤 `ZoneStateNotify` 대기 timeout | `run-1/`, 원래 run dir `/dev/shm/zlink-tmp-java/tmp.rk5rjzrOgY` |
| 변동 확인 | `ZW-A4,ZW-A5` | 통과 | `run-2/`, 원래 run dir `/dev/shm/zlink-tmp-java/tmp.Nfrvfn0MRy` |
| 재현 2 | `ZW-A1,ZW-A2,ZW-A3,ZW-A4,ZW-A5` | A1~A4 통과, A5 첫 join timeout | `prefix-1/`, 원래 run dir `/dev/shm/zlink-tmp-java/tmp.SGvLlKniuj` |

실패가 간헐적이어서 가장 작은 selector는 첫 실패 뒤 한 번 통과했다. full 실행과 같은 A5 join
실패를 다시 얻기 위해 앞선 A1~A4 부하를 포함한 prefix로 넓혔다. 두 실패 모두 A5 경계에서
발생했으며 assertion이나 timeout은 바꾸지 않았다.

첫 재현의 gateway와 zone-node-1 timeline은 다음과 같다.

| 시각 | node와 transition | 관찰 |
|---|---|---|
| 04:29:02.799 | gateway, 첫 A5 actor session seal 기록 | 첫 client의 actor binding이 열렸다. |
| 04:29:02.827 이후 | zone-node-1, 첫 A5 actor command 36 send 수락 | 약 100 ms 주기의 zone state push가 계속됐다. |
| 04:29:06.790 | gateway, 두 번째 A5 actor session seal 기록 | 두 번째 join도 server 쪽에서는 완료됐다. |
| 04:29:11.756 | gateway `zlink-stream-recv`, control send 실패 | `ZlinkSubmitException`이 수신 처리까지 전파되어 peer 3을 malformed frame으로 격리했다. 같은 격리가 peer 3~5에서 반복됐다. |
| 04:29:20.411 | gateway thread dump 1 | `dispatchControl → ZLinkJavaStreamSocket.send → inStateLane → CompletableFuture.join`에서 `WAITING`이었다. |
| 04:29:20.966, 04:29:21.421 | gateway thread dump 2~3 | `sendSessionClosing → send → inStateLane → join`에서 같은 `WAITING` 상태였다. |
| 04:29:24.393 | gateway thread dump 4 | 다시 heartbeat `dispatchControl`의 같은 stack에서 `WAITING`이었다. |
| client 종료 | A5 `Scenarios.java:113` | 두 player를 모두 포함한 `ZoneStateNotify`를 20초 안에 받지 못했다. |

zone-node-1과 zone-node-2의 같은 시각 thread dump에서는 gateway와 같은 STREAM 수신-owner 대기가
없었다. 두 node의 worker는 실행 가능하거나 queue 대기 상태였고, zone-node-1 log는 A5 actor의
command 36 send가 계속 수락됐음을 보여 준다. 따라서 멈춘 node는 actor owner가 아니라 client
STREAM을 소유한 gateway였다.

두 번째 재현에서는 04:33:01.550에 A5 session peer 7이 같은 synchronous control-send
`ZlinkSubmitException`으로 격리됐다. 직후 zone node가 보낸 A5 bound-session push는 gateway에서
rejected로 바뀌었고, client는 `ScenarioSupport.Game.join()`에서 timeout됐다. 04:33:03.563,
04:33:07.576, 04:33:10.582에 다른 session도 같은 경로로 격리되어 heartbeat 연쇄 실패가
첫 실패의 결과임을 확인했다.

## A5 경계와 원인

`ZW-A5`는 relocation이나 maintenance 시나리오가 아니다. 두 player를 같은 zone에 join한 뒤
`ZoneStateNotify`의 player 목록이 UTF-8 byte 순서이고 resident 값이 border copy보다 우선하는지
확인한다. 정의는 `framework/doc/framework/common/sample/zoneworld/README.ko.md:668`, Java
실행은 `samples/java/ZoneWorld/Client/.../Scenarios.java:106-120`에 있다. sample은 public
bound-session push 계약만 사용하므로 sample 수정 근거가 없었다.

원인은 하나의 synchronous wait 경계다.

- 수정 전 `ZLinkStreamRuntime.dispatchControl()`은 STREAM 수신 thread에서
  `stream.send(..., DONT_WAIT)`를 직접 호출했다. `sendSessionClosing()`과 heartbeat ping도 같은
  synchronous API를 사용했다. 실패 dump의 당시 위치는 `ZLinkStreamRuntime.java:1094,1458`이다.
- `ZLinkJavaStreamSocket.send()`는 `inStateLane()`을 호출하고
  `CompletableFuture.join()`으로 socket state lane의 결과를 기다렸다. 실패 dump의 위치는
  `ZLinkJavaStreamSocket.java:104,254`다.
- 기존 `sendAsync()`도 timeout을 읽는 `admissionTimeout() → nativeSocket()`과 실제 submit을
  `inStateLane()`으로 감싸 호출 thread를 기다리게 했다. 따라서 runtime 호출만 async API로
  바꾸면 동일한 대기가 남았다. 책임 코드는 현재 기준
  `ZLinkJavaSocketBacked.java:11-15`와 `ZLinkJavaStreamSocket.java:257-321`이다.
- A4/A5 actor의 주기적인 command 36 push가 같은 socket lane을 사용했다. control send를 호출한
  수신 owner가 lane 차례와 native completion을 기다리는 동안 새 client frame을 읽지 못했고,
  늦어진 heartbeat와 disconnect가 더 많은 stale push와 session 격리를 유발했다.

관련 계약은 `01-execution/02-handler-turn-and-execution-gate.ko.md`가 소유한다.

- `:145-153`: binding operation completion 같은 infrastructure 작업은 application turn과
  분리되어 진행된다.
- `:466-482`: application domain과 infrastructure domain은 독립적으로 진행해야 하며,
  application handler가 대기 중이어도 infrastructure task가 진행돼야 한다.

또한 `02-channel-transport/05-transport-liveness.ko.md:41-46`은 liveness signal을 application
handler와 분리하고 STREAM heartbeat를 별도 목적으로 규정한다. gateway의 유일한 STREAM 수신
owner가 application push와 같은 lane의 admission을 동기 대기한 동작은 이 경계를 지키지 못했다.

검토한 다른 방법은 synchronous `send(..., DONT_WAIT)`를 유지하는 방법과 control frame이 state
lane을 우회하는 방법이었다. 전자는 Java binding의 `submitSync`와 `join()`을 그대로 실행해
원인을 제거하지 못한다. 후자는 socket serialization을 깨뜨린다. 기존 backend async 계약을
사용하고 그 구현의 호출 시점 대기만 제거하는 방법이 public 경로와 frame 순서를 유지한다.

## 수정과 회귀 방지

- `ZLinkStreamRuntime.java:1062-1087,1426-1525`
  - heartbeat ping/pong과 `session-closing`을 `sendControlAsync()`로 통합했다.
  - receive owner와 liveness scheduler는 physical admission을 기다리지 않는다. 비동기 실패는
    control send 결과로 기록하며 수신 frame을 malformed로 분류하지 않는다.
  - payload는 admission stage가 끝날 때 닫는다.
  - `notifyServerDrain()`은 `session-closing` stage를 모두 합성하여 기존 drain 완료 의미를
    유지한다(`:1413-1423`).
- `ZLinkJavaStreamSocket.java:257-321`
  - no-timeout `sendAsync()`는 timeout 조회를 위해 먼저 `nativeSocket()` lane에 동기 진입하지
    않는다.
  - STREAM frame을 호출 thread에서 복사한 뒤 socket state lane에 submit 작업을 넣고 즉시
    flatten된 stage를 반환한다. lane 안에서 설정된 timeout을 읽고 기존
    `FrameworkStreamOperations.send()`를 호출한다.
  - frame은 native async operation을 만든 직후 닫으므로 caller payload와 frame 수명 계약을
    유지한다. 기본 1초와 명시적으로 설정된 admission timeout은 바꾸지 않았다.

회귀 test는 다음 실패 조건을 고정한다.

- `ZLinkJavaStreamSocketAsyncTerminalTest.asyncSendReturnsBeforeTheSocketStateLaneCanStartAdmission`
  (`:40-106`)은 socket lane의 선행 작업을 붙든 상태에서 `sendAsync()` 호출이 lane 해제 전에
  stage를 반환하는지 확인한다.
- `ZLinkStreamRuntimeIngressTest.pendingHeartbeatPongAdmissionDoesNotBlockTheReceiveOwner`
  (`:258-275`)은 heartbeat pong admission stage를 미완료로 유지한 상태에서도 다음 peer의
  application frame이 dispatch되는지 확인한다. synchronous pong send가 한 번도 호출되지
  않았음도 확인한다.

## 검증 결과

모든 sample 검증은 `/dev/shm` run directory, `ZLINK_JAVA_STREAM_TRACE=1`, run-dir 보존 설정과
지정된 JVM gate lock을 사용했다. green client/runner log는
`zlink-work/c016/logs/bucketB-java-zoneworld-3-green/`에도 보존했다.

| 검증 | 결과 | run dir 또는 근거 |
|---|---|---|
| 관련 test class | `BUILD SUCCESSFUL` | `ZLinkJavaStreamSocketAsyncTerminalTest`, `ZLinkStreamRuntimeIngressTest` |
| selector 1 | A4, A5 통과, exit 0 | `/dev/shm/zlink-tmp-java/tmp.8IrEjYL5Rt` |
| selector 2 | A4, A5 통과, exit 0 | `/dev/shm/zlink-tmp-java/tmp.zXiqiarRb7` |
| selector 3 | A4, A5 통과, exit 0 | `/dev/shm/zlink-tmp-java/tmp.GazXL9BXRH` |
| FULL 1 | 전체 scenario 통과, `zoneworld=completed`, exit 0 | main `/dev/shm/zlink-tmp-java/tmp.k9vTT4owxl`; G4 child `tmp.2m8oGNDies`; B8 child `tmp.p7KufZdvfP` |
| FULL 2 | 전체 scenario 통과, `zoneworld=completed`, exit 0 | main `/dev/shm/zlink-tmp-java/tmp.1GoNQMiB7o`; G4 child `tmp.9x1J9r1tYP`; B8 child `tmp.W933TcxVLG` |
| Java core 전체 test | 1,215개 중 기존 M6A 2개만 실패 | `:zlink-framework-core:test` 1회 실행 |

Java core 전체 test의 남은 실패는 다음 기존 descriptor-fence test다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`

## 변경 파일

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java`
- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocketAsyncTerminalTest.java`
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntimeIngressTest.java`
- `doc/plan/c016-worklog/bucketB-java-zoneworld-3-summary.md`

## BLOCKERS

- 기능 blocker는 없다. selector 3회와 FULL 2회가 모두 통과했다.
- 요청에 적힌 `zoneworld-placement=completed` 문자열은 현재 repository에서 job prompt에만
  존재한다. 현행 Java ZoneWorld runner가 정의하고 출력하는 전체 성공 marker는
  `run_sample.sh:333-340`의 `zoneworld=completed`다. sample 계약을 바꾸지 않고 이 marker와 exit
  0을 FULL 성공 근거로 사용했다.
- Java core 전체 gate는 위의 기존 M6A 두 실패 때문에 green이 아니다. 이번 변경은 descriptor
  replacement 경로를 수정하지 않았다.
- 기존 사용자 변경과 다른 언어, `core/**`, `bindings/**`, 보호된 spec·sample 문서는 수정하지
  않았고 commit하지 않았다.
