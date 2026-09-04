# Java GameQuest active binding 실패 조사

## 결론

원인은 STREAM session liveness 주기 작업이 transport 종료 중 발생한
`ZlinkSubmitException(SubmitResult.NOT_CONNECTED)`를 밖으로 내보낸 것이다.
`scheduleAtFixedRate`에 등록한 작업은 한 번 예외로 끝나면 다음 실행이 취소된다. 따라서 이후 session의
heartbeat timeout, automatic Actor binding cleanup, `GameQuestSession.onDisconnected()`,
`RedisSampleStore.unbind("player-alice")`가 모두 실행되지 않았고 server evidence가
`failure:unexpected:active-binding:player-alice`를 반환했다.

원인 위치는
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java:1481`의
heartbeat ping 제출이다. 수정 전에는 이 제출 실패를 처리하지 않았다.

## Transition trace

1. `JoinSessionReq("player-alice")`가 session을 만들고 Actor를 bind했다. 실패 재현 로그에서
   `boundSessionBind(38)`이 새 binding을 설치했고 모든 ROUTER request는 `SubmitResult.OK`였다.
2. Client가 original, reconnect, scale connector를 차례로 닫았다. transport가 닫힌 routing ID가
   liveness snapshot에는 잠시 남아 있었다.
3. `ZLinkStreamRuntime.checkSessionLiveness()`가 아직 timeout 전인 stale session에 heartbeat ping을
   보냈다. 동기 제출이 `NOT_CONNECTED`를 던졌고 주기 작업이 그 자리에서 종료됐다.
4. 다음 liveness tick이 없었으므로 5초가 지나도 session을 제거하지 않았고
   `disconnectSessionStage()`도 시작하지 않았다. 임시 계측에서도 실패 판정 전에는 disconnect cleanup
   또는 Session disconnect callback이 없었고, process teardown 뒤에만 callback 진입이 보였다.
5. Client는 connector close 뒤 최대 10초 동안 `/self-check/assert`를 다시 조회했지만
   `GameQuestStore.assertState()`의 active binding 검사가 계속 실패했다.
6. 수정 후에는 transport teardown의 `ZlinkSubmitException`만 제한적으로 기록하고 다음 tick을
   유지한다. 다음 tick이 만료 session을 제거하고 automatic binding disconnect와 local cleanup을
   마친 뒤 Session callback을 호출한다. callback이 sample store에서 `player-alice`를 unbind하여
   evidence가 통과한다.

## 계약 판정

`04-session/02-session-actor-binding.ko.md` §7, 385-396행은 Framework가 physical disconnect를
관찰하면 current binding snapshot마다 disconnect를 자동 제출하고, 한 제출이나 callback이 실패해도
나머지 통지와 Session cleanup을 계속해야 한다고 정한다. 한 heartbeat 제출 실패가 전체 liveness
주기와 이후 cleanup을 영구 중단한 기존 동작은 이 all-settled 계약에 맞지 않는다.

GameQuest의 `GameQuestSession.onDisconnected()`가 `playerActor.notifyDisconnected()`를 다시 호출하는
별도 계약 불일치도 확인했다. §7 385-388행은 이 순회를 Framework가 맡고 application Session callback은
bound Actor를 순회하지 않는다고 정한다. 다만 sample callback만 고친 격리 실행은 실패했고, sample을
원복한 뒤 runtime 수정만 둔 실행은 통과했으므로 이번 실패의 원인은 아니며 변경에 포함하지 않았다.

## 수정과 회귀 테스트

- `ZLinkStreamRuntime.sendHeartbeatPing()`이 transport 종료 중의 `ZlinkSubmitException`을 처리한다.
  예상된 transport 경계만 처리하며 다른 runtime 예외는 숨기지 않는다.
- `ZLinkStreamRuntimeIngressTest.heartbeatTransportFailureDoesNotStopLivenessChecks()`를 추가했다.
  첫 heartbeat ping에서 `NOT_CONNECTED`를 발생시킨 뒤 session timestamp를 만료시키고, 다음 주기에서
  session-closing 전송이 일어나는지 확인한다.
- 방어 코드를 제거한 red 실행: `BUILD FAILED`, assertion 271행.
- 방어 코드를 복원한 focused 실행: `BUILD SUCCESSFUL`.

## 실행 결과

- 수정 전 현재 `main` 재실행 2회: 2/2 실패, 모두 `GameQuestClientScenario.java:215`.
  보존 경로: `/dev/shm/zlink-tmp-java/tmp.VYWVqz8oCD`,
  `/dev/shm/zlink-tmp-java/tmp.HVAhpdvRnD`.
- sample callback 수정만 둔 격리 실행: 실패.
  보존 경로: `/dev/shm/zlink-tmp-java/tmp.ybxNX5ieRW`.
- runtime 수정만 둔 최종 상태: 3/3 성공, exit 0. 세 실행 모두
  `gamequest-server-evidence=completed`, `gamequest=completed`,
  `gamequest-placement=completed`를 출력했다.
  보존 경로: `/dev/shm/zlink-tmp-java/tmp.9HpiEy9kpf`,
  `/dev/shm/zlink-tmp-java/tmp.CkKjxn5lwE`,
  `/dev/shm/zlink-tmp-java/tmp.PEwALOCPRC`.
- focused regression: 성공.
- `:zlink-framework-core:test`: 1,214개 중 1,212개 성공, 2개 실패.

## BLOCKERS

전체 core gate의 아래 두 실패가 남았다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`

두 실패는 동시에 진행 중인 ZoneWorld 작업이 수정한 Java binding 파일에서 발생했다. 이번 변경 파일과
다른 범위이며 요청에 따라 수정하지 않았다. GameQuest focused regression과 sample 3회에는 남은 실패가
없다.
