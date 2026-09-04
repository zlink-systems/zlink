# Bucket B — Java ZoneWorld `ZW-A2` 2차 조사 결과

## 결론

`ZW-A2`의 약 1초·2초 service completion 지연 원인은 binding의 completion wake 누락이
아니었다. gateway의 유일한 public mesh poller 소유 스레드가 service command 36을 dispatch한
뒤, 같은 호출 스택에서 STREAM bound-session send의 물리 admission을 동기 대기했다. 이 동안
스레드는 `poller.wait()`로 돌아가지 못했으므로 이미 제출된 mesh REQUEST completion도 drain할
수 없었다. completion 관찰 시점이 command 36 dispatch 복귀 시점과 매번 0~2 ms 이내로 맞물린
것도 이 결론과 일치한다.

수정은 command 36의 application-side admission을 `CompletionStage<Boolean>`으로 만들고 raw
mesh pump가 그 stage를 기다리지 않게 했다. 현재 binding의 STREAM 전송은 session state lane에서
순서대로 `sendBoundSessionPushAsync()`에 제출하되, 앞 전송의 물리 admission 완료가 다음 제출을
막지 않는다. Core가 소유하는 같은 session FIFO와 async-only one-way admission 계약은 유지된다.

수정 후 `G1/G5/A1/A2` prefix를 4회 실행해 모두 통과했고, A2 gateway 구간은 28~37 ms였다.
관련 회귀 test도 통과했다. full 실행 1회에서는 A2와 A3/A4까지 통과했지만 별도 A5 client
timeout이 첫 실패였고 이후 heartbeat 실패가 연쇄됐다. 첫 실제 실패 뒤 동일한 full gate를
반복하지 않았으므로 요청된 full 2회 green은 달성하지 못했다.

## 측정

측정 실행은 먼저 기존 message flow `NORMAL`과 `ZLINK_JAVA_STREAM_TRACE=1`을 켰다. 여기에
Framework가 native stage를 제출하고 관찰한 시각과 raw mesh pump의 dispatch 체류 시간을 찍는
임시 probe를 추가했다. binding 파일은 이 job의 수정 범위 밖이므로 직접 변경하지 않았다.
probe는 조사 뒤 모두 제거했다.

SUB `POLLCOMPLETION` 수정이 적용된 상태에서 실패한 실행의 gateway timeline은 다음과 같다.
로그는
`zlink-work/c016/logs/bucketB-java-zoneworld-2-measurement/pre-fix-run/gateway.log:118-176`에
보존했다.

| 시각 | 경계 | 직전 경계부터 | 관찰 |
|---|---|---:|---|
| 03:36:21.931 | A2 `JoinWorldMsg` received | 기준 | admitted/dispatched도 같은 ms |
| 03:36:21.938 | actor-create mesh request 시작 | 7 ms | Redis reserve까지 완료 |
| 03:36:21.939 | native REQUEST stage id 15 제출 | 1 ms | submit은 정상 반환 |
| 03:36:22.931 | raw mesh command 36 dispatch 복귀 | 2.001 s 체류 | poller owner가 application delivery를 기다림 |
| 03:36:22.933 | id 15 continuation 관찰 | 제출 후 993.566 ms | pump 복귀 2 ms 뒤 |
| 03:36:22.936 | bound-actor REQUEST stage id 18 제출 | 3 ms | submit은 정상 반환 |
| 03:36:24.932 | 다음 command 36 dispatch 복귀 | 1.999 s 체류 | poller가 다시 application delivery를 기다림 |
| 03:36:24.934 | id 18 continuation 관찰 | 제출 후 1.997862 s | pump 복귀 2 ms 뒤 |
| 03:36:24.935 | 후속 actor send REQUEST id 20 제출 | 1 ms | A2 응답 경로의 다음 admission |
| 03:36:27.933 | 다음 command 36 dispatch 복귀 | 2.996 s 체류 | 같은 패턴 반복 |
| 03:36:27.934 | id 20 continuation 관찰 | 제출 후 2.999829 s | pump 복귀 1 ms 뒤 |
| 03:36:27.936 | stream flow completed | received 후 6.005 s | 3초 client wait를 초과 |

### Thread state

03:36:21.949, 03:36:22.276, 03:36:22.492에 얻은 세 thread dump는
`zlink-work/c016/logs/bucketB-java-zoneworld-2-measurement/thread-dumps/`에 보존했다. 세 번 모두
동일했다.

- `zlink-jvm-raw-mesh-zoneworld.mesh`(tid 130)는 `CompletableFuture.join()`에 parked 상태였다.
  호출 스택은
  `ZLinkJavaRawMeshNode.dispatchBoundSessionSend → ZLinkStreamRuntime.handleBoundSessionSend →
  ZLinkSessionActorsRuntime.acceptBoundSessionSend → deliverCurrentBoundSessionSendLocked →
  ZLinkJavaStreamSocket.sendBoundSessionPush → inStateLane`이었다.
- STREAM socket state lane의 virtual thread(tid 43630)는
  `ZLinkStateLane.runNext → ZLinkJavaStreamSocket.sendBoundSessionPushOnLane →
  ZLinkJavaSocketSupport.submitSync → MessageOperations.SendBuilder.submit_sync →
  CompletionOwner.submitSendBlocking → Native.sendPartRid` 안에 있었다.
- 따라서 public mesh poller owner는 completion을 기다리며 `poller.wait()`에 있던 것이 아니라,
  command 36 application delivery가 STREAM backpressure에서 끝나기를 기다렸다. 별도 liveness와
  state-lane 작업이 동작한 사실은 process 전체 정지가 아니었음을 함께 보여 준다.

직접 관찰한 경계는 Framework의 native stage 제출/continuation과 public pump 복귀다. Core 내부
completion enqueue 시각을 직접 계측하지는 않았으므로 이를 별도 사실로 주장하지 않는다.
다만 세 지연 모두 continuation이 pump 복귀 직후 발생했고 thread dump에서 owner가 poller 밖에
있었으므로, 이 실패를 설명하는 데 lost wake 가설은 필요하지 않다.

## 원인과 계약

원인 코드는 수정 전 다음 경로였다.

- `ZLinkJavaRawMeshNode.dispatchBoundSessionSend()`가 command 36 handler의 boolean 결과가 나올
  때까지 raw mesh pump 스택을 유지했다.
- `ZLinkStreamRuntime.handleBoundSessionSend()`가 session owner의 synchronous `accept()`를
  호출했다.
- `ZLinkSessionActorsRuntime.acceptBoundSessionSend()`의 current-binding 경로가
  `deliverCurrentBoundSessionSendLocked()`를 거쳐 `sendBoundSessionPush()`의 state-lane 결과를
  동기 대기했다.

현재 파일 기준 책임 경계는
`ZLinkJavaRawMeshNode.java:6357-6387`, `ZLinkStreamRuntime.java:471-485`,
`ZLinkSessionActorsRuntime.java:1193-1222,1346-1357`이다.

이 동작은 execution 계약의 두 조항을 어겼다.

- `01-execution/02-handler-turn-and-execution-gate.ko.md:15-29`: binding operation completion과
  호출 완료 확정 같은 infrastructure 작업은 application 대기와 무관하게 진행한다.
- 같은 문서 `:466-478`: application handler가 대기 중이어도 infrastructure task를 진행할 수
  있어야 한다.

또한 `00-foundation/04-interaction-model.ko.md:420-433`은 bound session send가 current binding의
session FIFO를 사용하면서 async-only one-way admission 결과를 반환하도록 정한다. command 36의
물리 STREAM admission을 public mesh receive owner가 동기 대기한 기존 경로는 이 계약에도 맞지
않았다.

## 수정과 회귀 방지

- `ZLinkInternalMeshNode.BoundSessionSendHandler`의 결과를 `CompletionStage<Boolean>`으로
  변경했다(`ZLinkInternalMeshNode.java:719-725`).
- raw mesh pump는 handler stage에 관찰 callback만 연결하고 즉시 dispatch를 끝낸다. application
  settlement가 public mesh poller의 다음 wait/drain을 막지 않는다
  (`ZLinkJavaRawMeshNode.java:6367-6387`).
- `ZLinkStreamRuntime`은 session 목록 조회와 match를 기존 state lane 순서 안에서 수행한 뒤
  실제 owner의 비동기 admission stage를 flatten한다
  (`ZLinkStreamRuntime.java:471-485,503-516,562-570`).
- `ZLinkSessionActorsRuntime`은 actor state lane에서 current/relocation binding을 판정한다. current
  binding은 `sendBoundSessionPushAsync()`에 제출하고, relocation target은 기존 bounded FIFO에
  admit한 뒤 기존 drain을 시작한다
  (`ZLinkSessionActorsRuntime.java:1193-1222,1346-1357`).
- synchronous helper와 demux 경로는 기존 내부 test 호출을 위해 유지했지만 production command
  36 handler는 비동기 경로를 사용한다.

회귀 test는 두 가지 실패 조건을 고정한다.

- `ZLinkJavaRawSpotNodeM6BTest.remoteBoundStreamUsesRawMeshAndExactGenerationFences`
  (`:2573-2715`)는 첫 command 36 handler settlement를 미완료로 붙든 상태에서도 두 번째
  command가 poller에서 dispatch되는지 확인한다.
- `ZLinkSessionActorBindingContractTest.asyncBoundSessionSendSubmitsInSessionFifoWithoutWaiting`
  (`:222-253`)는 두 physical admission을 미완료로 붙들고도 `first`, `second` 순서의 두 제출이
  모두 시작되는지 확인한다. assertion이나 timeout은 낮추지 않았다.

## 검증 결과

모든 sample 실행은 `/dev/shm` run directory, `ZLINK_JAVA_STREAM_TRACE=1`, message flow `NORMAL`,
run-dir 보존 설정을 사용했다.

| 검증 | 결과 | 근거 |
|---|---|---|
| 관련 unit tests | 통과, `BUILD SUCCESSFUL` | `bucketB-java-zoneworld-2-focused-test.log` |
| prefix 1 | G1/G5/A1/A2 통과; A2 30 ms | `bucketB-java-zoneworld-2-green-prefix-1.log`, role-log 디렉터리 |
| prefix 2 | G1/G5/A1/A2 통과; A2 28 ms | `bucketB-java-zoneworld-2-green-prefix-2.log`, role-log 디렉터리 |
| prefix 3 | G1/G5/A1/A2 통과; A2 37 ms | `bucketB-java-zoneworld-2-green-prefix-3.log`, role-log 디렉터리 |
| prefix 4 | G1/G5/A1/A2 통과; A2 34 ms | `bucketB-java-zoneworld-2-green-prefix-4.log`, role-log 디렉터리 |
| full 1 | G4/B8 child 통과; G1/G2-rid/G5/A1~A4 통과; 첫 실패 A5 | `bucketB-java-zoneworld-2-green-full-1.log` 및 `-run-{1,2,3}` role logs |
| Java core test | 1,213개 중 기존 M6A 2개 실패 | `bucketB-java-zoneworld-2-core-test.log` |

수정 후 4회 A2 상세 시간은 다음과 같다. 각 값은 gateway message-flow received를 기준으로 한다.

| 실행 | mesh request | mesh response | bound actor accepted | stream completed |
|---|---:|---:|---:|---:|
| 1 | +7 ms | +17 ms | +29 ms | +30 ms |
| 2 | +8 ms | +17 ms | +27 ms | +28 ms |
| 3 | +8 ms | +18 ms | +36 ms | +37 ms |
| 4 | +11 ms | +22 ms | +32 ms | +34 ms |

관련 test 명령은 다음 세 test를 한 Gradle 실행에서 수행했고 성공했다.

- `ZLinkSessionActorBindingContractTest.asyncBoundSessionSendSubmitsInSessionFifoWithoutWaiting`
- `ZLinkJavaRawSpotNodeM6BTest.remoteBoundStreamUsesRawMeshAndExactGenerationFences`
- `ZLinkStreamRuntimeBoundSessionSendDemuxTest`

`:zlink-framework-core:test`의 남은 실패는 다음 두 기존 descriptor-fence test뿐이다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`

## BLOCKERS

- full 1은 A2를 포함해 A4까지 통과한 뒤 `Scenarios.a5()`의 client join에서
  `TimeoutException`으로 처음 실패했다. 이후 B1/B2/B3/B5/B6/B7 등의 heartbeat timeout이
  연쇄됐고 최종 marker는 `ZW-A5` 때문에 withheld되었다. A2 service completion 재발 증거는
  없으며, 이번 한 원인 범위와 다른 장시간 시나리오 실패다. 첫 실패가 바뀌지 않은 상태에서
  같은 전체 gate를 반복하지 않는 규칙에 따라 full 2회째는 실행하지 않았다.
- Java core 전체 gate는 위의 기존 M6A 두 실패 때문에 green이 아니다. 이 job은 descriptor
  replacement 경로를 수정하지 않았다.
- 사용자 작업인 다른 언어·binding 변경은 건드리지 않았고 commit하지 않았다. 별도 job의
  `ZLinkJavaSocketReceivePoller`와 `ZLinkJavaSubscriberSocket`도 수정하지 않았다.
