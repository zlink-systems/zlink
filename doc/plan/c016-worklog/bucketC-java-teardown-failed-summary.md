# Java sample role teardown 실패 조사

## 결과

Java STREAM session의 remote Actor binding을 종료하는 request가 이미 닫힌 service
ROUTER에 제출되면 `SubmitResult.NOT_CONNECTED`가 동기 예외로 반환된다. 기존 코드는 같은
결과가 `CompletionStage`에서 비동기로 반환될 때만 정상적인 중복 정리 결과로 인정했다.
동기 예외는 실제 local binding 정리를 끝낸 뒤에도 host teardown 실패로 전달되어
`ForceStopped/TeardownFailed`를 만들었다.

`ZLinkJavaStreamSocket`이 동기 submit failure에도 기존의 idempotent unbind 분류를 적용하도록
수정했다. TicTacToe, SupportChat, DeliveryDispatch는 각각 2회 모두 placement marker와 exit
code 0을 확인했다.

## 실패 transition

1. Host는 `ZLinkFrameworkRuntime.java:1971-2023`에서 teardown action을 역순으로 실행한다.
   `streams.closeAsync()`가 첫 action이다(`:2020-2022`).
2. `ZLinkJavaStreamSocket.java:541-603`은 현재 session binding snapshot마다 250 ms
   unbind를 요청한다. Remote Actor binding은 `ZLinkJavaRawSpotNode.java:2202-2231`에서
   `active=false`인 `COMMAND_BOUND_SESSION_BIND` request로 바뀌며,
   `ZLinkJavaRawMeshNode.java:2866-2941`이 service ROUTER에 제출한다. Command 값은
   `framework/runtime/protocol/generated/jvm/ServiceWireConstants.java:37`의 `38`이다.
3. 상대 role이 먼저 transport를 닫은 실행에서는 `ZLinkJavaRawServicePort.java:144-190`의
   `port.request(...)`가 stage를 반환하기 전에
   `ZlinkSubmitException(SubmitResult.NOT_CONNECTED, errno=113)`을 던졌다. 최초 계측 재현은
   `courier-session`의 `ZLinkJavaStreamSocket.java:609`에서
   `STREAM binding cleanup did not complete`를 기록했고 cause stack은
   `ZLinkJavaRawServicePort.java:149,175`까지 이어졌다. Native 값은 제공된
   `zlink-work/c016/logs/bucketB-java-delivery-run2/courier-session.log:158-161`의
   `result=2 (NOT_CONNECTED), errno=113` probe와 일치한다.
4. 기존 `ZLinkJavaStreamSocket.java:559-573`은 stage에서 비동기로 끝난
   `NOT_CONNECTED`를 `isRemoteRouteUnavailable()`로 허용했지만, 기존 동기 catch는 모든
   예외를 failed future로 바꿨다. `allOf()`의 실패는 local binding을 제거하는 `finally`가
   끝난 뒤 `IllegalStateException`으로 전달됐다.
5. `ZLinkFrameworkShutdown`이 이 예외를 teardown failure로 수집하고,
   `ZLinkFrameworkRuntime.java:1482-1486`이 이를
   `FORCE_STOPPED/TEARDOWN_FAILED`로 변환했다.

Application message-flow는 마지막 functional marker까지 모두 완료됐다. 이 요청은 STREAM
application dispatch가 아니라 shutdown의 service control request이므로 message-flow 이후의
`ZLINK_JAVA_STREAM_TRACE`와 임시 예외 계측으로 transition을 확인했다. 임시 계측 코드는
조사 뒤 제거했다. 재현 로그는
`zlink-work/c016/logs/bucketC-java-teardown-diagnosis/`에 보관했다.

## 계약 판정

기준 계약은 `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md`
§14이다.

- `:766-776`은 accepted work와 session barrier를 처리한 뒤 Spot, Actor, owner,
  descriptor, listener, transport를 정리하고 deadline 안에 끝나면 `Stopped/None`을
  반환하도록 정한다. 실제 cleanup이나 callback이 실패하면
  `ForceStopped/TeardownFailed`를 사용한다(`:823-826`).
- `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:295-300`은
  shutdown 중 필요한 connection을 deadline까지 유지할 수 있지만, 종료 시 timer,
  subscription, pending callback을 끝낸 뒤 connection을 닫도록 정한다. 여러 role이 함께
  종료되므로 remote route가 먼저 닫혀 unbind tombstone을 받지 못하는 상태는 정상적인
  connection-loss 경쟁이다.
- Local binding 제거는 failure와 관계없이 `ZLinkJavaStreamSocket.java:594-601`에서 끝난다.
  따라서 이미 닫힌 remote route의 `NOT_CONNECTED`는 미완료 local teardown이 아니며,
  `Stopped/None`을 막을 근거가 없다.

Java exact interface의 유효 값은
`framework/doc/framework/common/spec/server/languages/java/interfaces/common-runtime.ko.md:60-76`이며,
공통 lifecycle 설명은 `ForceStopped`가 별도 state가 아니라 bounded teardown 결과라고
정한다(`05-host-relocation-flow.ko.md:242-245`).

## 다른 언어와의 parity

| Runtime | Shutdown-time STREAM binding cleanup |
|---|---|
| Node.js | `managed-stream.ts:46-49`는 `NotConnected`와 `NotFound`를 idempotent unbind terminal로 정의한다. Transport가 이미 닫혔으면 native unbind 자체를 생략하고 local binding을 제거한다(`:307-337`). |
| .NET | Transport close의 binding snapshot notification은 all-settled best effort이며 remote notification exception을 trace한 뒤 local tombstone을 항상 제거한다(`ZLinkSessionActorBindingRegistry.cs:92-159`). Remote disconnect가 host teardown failure가 되지 않는다. |
| C++ | STREAM server stop은 `close_core_sessions()`와 `begin_core_session_close()`를 `noexcept` 경계에서 실행한다(`stream_host_service.cpp:2399-2451,2757-2790`). Session/Actor/scope close failure를 흡수하고 local retirement를 끝내므로 사라진 peer route가 host teardown failure로 승격되지 않는다. |
| Java 수정 후 | 비동기 failure와 동기 submit failure 모두 기존 `isStaleBinding()`/`isRemoteRouteUnavailable()` 판정을 통과한다(`ZLinkJavaStreamSocket.java:553-580,785-804`). 실제 timeout, unexpected exception, native close failure는 계속 실패로 보존한다. |

## 변경과 회귀 test

- `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java:575-580`
  - 동기 unbind 예외에도 비동기 completion과 같은 stale/remote-unavailable 판정을 적용했다.
  - Assertion, timeout과 예상하지 않은 runtime failure의 기존 실패 처리는 유지했다.
- `framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNodeM6BTest.java:2238-2285`
  - `BoundSessionLifecycle.unbind()`가 stage 반환 전에
    `ZlinkSubmitException(SubmitResult.NOT_CONNECTED)`을 던지는 실제 failure 형태를 재현한다.
  - `stream.close()`가 정상 완료되는지 검증한다. 기존 assertion은 낮추지 않았다.

Focused test:

```text
:zlink-framework-core:test
--tests ZLinkJavaRawSpotNodeM6BTest.streamCloseCompletesWhenRemoteBindingRouteFailsDuringSubmit
BUILD SUCCESSFUL (1 test)
```

## Sample 검증

모든 실행은 `TMPDIR=/dev/shm/zlink-tmp-java`, 지정된 0.17.0 `libzlink.so`,
`flock -w7200 /tmp/zlink-jvm-gate.lock`을 사용했다. Runner는 placement marker를 출력하기 전에
각 role log의 `ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE`을 검사하므로 exit 0과
marker가 clean lifecycle을 함께 증명한다(`framework/languages/java/samples/runner-common.sh:131-169`).

| Sample | Run 1 | Run 2 |
|---|---|---|
| TicTacToe | exit 0, `tictactoe-placement=completed` | exit 0, `tictactoe-placement=completed` |
| SupportChat | exit 0, `supportchat-placement=completed` | exit 0, `supportchat-placement=completed` |
| DeliveryDispatch | exit 0, `deliverydispatch-placement=completed` | exit 0, `deliverydispatch-placement=completed` |

Runner log는 `zlink-work/c016/logs/bucketC-java-teardown-verification/`에 보관했다.

## Java core gate

요청한 전체 gate를 한 번 실행했다.

```text
./gradlew --no-daemon :zlink-framework-core:test
1210 tests completed, 2 failed
```

이번 회귀 test와 관련 class는 통과했다. 실패는 제공된 이전 gate 요약과 동일한 M6A 두 건이다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
  - `ZLinkJavaRawMeshNodeM6ATest.java:1400`: `peer state was not observed: CLOSED`
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`
  - `ZLinkJavaRawMeshNodeM6ATest.java:1439`: expected `true`, actual `false`

전체 gate log는
`zlink-work/c016/logs/bucketC-java-teardown-verification/core-test.log`에 보관했다.

## BLOCKERS

- 이 teardown 수정과 6회 sample 검증에는 blocker가 없다.
- Java core 전체 gate green에는 위 M6A 2건이 남아 있다. 이 작업의 STREAM cleanup 변경과
  다른 test/class이며, 원인이 바뀌지 않은 기존 failure이므로 범위 밖에서 수정하거나
  assertion을 낮추지 않았다.
