# Bucket B — Java ZoneWorld `ZW-A2` 조사 결과

## 결론

실패한 `ZW-A2`에서 gateway는 `JoinWorldMsg`를 받은 즉시 수락하고 handler에 전달했으며,
Redis 예약도 4 ms 안에 마쳤다. 그 뒤 actor 생성 요청의 완료가 돌아오기까지 994 ms, 생성한
actor로 보내는 요청의 local admission 완료가 돌아오기까지 다시 2.008 s가 걸렸다. gateway
내부 구간은 합계 3.011 s였다. 같은 시간에 gateway의 mesh 처리기는 liveness probe와 다른
service 명령을 계속 처리했다. 따라서 stream 입구의 대기, Redis 재시도, `JoinWorldMsg`
handler 진입 지연, 한 actor의 handler가 실행 자원을 계속 점유한 상태는 관찰되지 않았다.

조사 중 Java receive poller가 completion queue가 없는 SUB socket에도
`POLLCOMPLETION`을 등록하는 별도 결함을 확인했다. 이 결함은 zone node마다 약 10 ms 간격의
`NOT_SUPPORTED` 예외를 만들었지만, 이를 제거한 실행에서도 A2 gateway 구간이 6.003 s까지
늘어나 실패했다. 반복 예외는 불필요한 부하를 만들지만, 예외가 없는 실행에서도 A2 지연이
남았으므로 보고된 timeout의 주원인으로 볼 수 없다. 한 원인 제한에 따라 이 변경과 회귀
test는 최종 작업 트리에서 되돌렸다.

현재 원인은 확정하지 못했다. trace로 확인한 마지막 경계는 Java Framework가 service
request/send를 호출한 시점부터 native stage가 admission 완료를 알린 시점까지다. 이 경계
안에서 completion 처리가 늦었다는 것은 관찰했지만, completion 소유 방식이 원인이라는
판단은 아직 가설이다. assertion과 timeout은 변경하지 않았고, 검증하지 못한 runtime 수정도
남기지 않았다.

관련 실행 계약은 infrastructure 작업을 application handler의 대기와 무관하게 진행하도록
요구하며, 그 작업에 binding operation completion과 호출 완료 확정이 포함된다
(`framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:15-29`,
`:466-478`). 또한 ZoneWorld의 `JoinWorldRes`는 target zone admission이 끝난 뒤에만 보내야 한다
(`framework/doc/framework/common/sample/zoneworld/README.ko.md:426-434`). 따라서 sample이 응답을
앞당겨 timeout을 감추는 방식은 계약에 맞지 않는다.

## Gateway 시간 분해

실패한 두 번째 prefix의 flow는
`zlink-work/c016/logs/bucketB-java-zoneworld-current-prefix2/gateway.log:88-117`에 있다.

| 구간 | 시각 | 소요 시간 | 관찰 |
|---|---:|---:|---|
| stream received | 02:25:22.123 | 기준 | `JoinWorldMsg` 수신 |
| admitted | 02:25:22.123 | 0 ms | ingress backlog 없음 |
| dispatched | 02:25:22.124 | 1 ms | session handler가 즉시 시작됨 |
| actor target 선택 | 02:25:22.127 | 3 ms | 후보 2개 중 node 선택 |
| Redis reserve 완료 | 02:25:22.131 | 4 ms | location store retry 지연 없음 |
| mesh request 호출 | 02:25:22.131 | 0 ms | Framework가 native REQUEST를 호출함 |
| mesh actor-create 응답 | 02:25:23.125 | 994 ms | 첫 지연 구간 |
| bound actor send 수락 | 02:25:25.133 | 2.008 s | 두 번째 지연 구간 |
| actor flow sent | 02:25:25.134 | 1 ms | 합계 3.011 s |

첫 번째 prefix에서 같은 A2 경로는
`gateway.log:103-141`의 `received 02:23:55.693 → completed 02:23:55.730`, 즉 37 ms에
끝났다. actor 생성은 02:23:55.694에 시작했고 target 선택은 3 ms 뒤, Redis 예약 완료는
4 ms 뒤였다. mesh actor-create 응답은 요청 후 11 ms, actor send 완료는 그 뒤 18 ms 안에
도착했다. 정상 실행과 실패 실행 모두 Redis 및 sample handler 앞부분은 3초 지연을 만들지
않았다.

느린 구간 동안 gateway는 02:25:23.124에 ops와 다른 zone node로 liveness probe를 보냈고,
02:25:23.125 및 02:25:25.137에 service command 36을 받았다
(`gateway.log:101-105`, `:121-122`). gateway process가 멈춘 것이 아니라 service transport의
개별 completion이 늦었다.

## 확인한 runtime 결함과 제외 근거

`4d263e66`에서 `ZLinkJavaSocketReceivePoller.ensureRegistered()`가 모든 socket에
`POLLIN|POLLOUT|POLLCOMPLETION`을 등록하도록 바뀌었다
(`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketReceivePoller.java:40-54`).
SUB socket은 completion queue를 제공하지 않는다. Core polling 계약도
`POLLCOMPLETION`을 PAIR·DEALER·ROUTER·STREAM에만 허용하고, 지원하지 않는 source event는
`NOT_SUPPORTED/ENOTSUP`으로 끝내도록 정한다
(`core/doc/spec/core/05-polling.ko.md:85-90`, `:282-284`).

그 결과 두 번째 prefix에서 `ZLinkFanoutLocationRuntime`이 다음 횟수만큼 같은 예외를 남겼다.

| process | `fanout location tick failed` 횟수 |
|---|---:|
| zone-node-1 | 3,262 |
| zone-node-2 | 3,006 |
| zone-node-3 | 2,144 |

첫 stack은
`zlink-work/c016/logs/bucketB-java-zoneworld-current-prefix2/zone-node-1.log:19-39`이며
`SocketCore.completionOwner()`에서 실패한다. SUB poller에는 `POLLIN`만 등록하는 임시 수정과
`subscriberReadinessDoesNotClaimAnUnsupportedCompletionQueue` 회귀 test를 작성했고, 해당 test는
통과했다. 그러나 같은 수정 상태의 최종 prefix에서도 A2는 실패했다. gateway는
03:14:01.221에 A2 처리를 시작해 actor send를 03:14:07.224에 수락했다
(`/dev/shm/zlink-tmp-java/tmp.KS2bojRaXL/logs/gateway.log:91-118`). SUB 예외는 0건이었다.
따라서 반복되는 poller 예외는 A2 timeout의 주원인이 아니며, 임시 수정도 되돌렸다.

별도 실험으로 native completion과 Framework continuation 사이에 비동기 handoff를 삽입한
실행에서는 prefix가 한 차례 통과했다. 그러나 반복 실행은 remote `JoinSpot`의 `DATA_LOST`
또는 A2 timeout으로 실패했다. 검증되지 않은 이 변경도 되돌렸다. 다음 조사가 필요한 코드는
`ZLinkJavaRawServicePort.sendOnLane()/requestOnLane()`이 반환하는 native stage와 public poller의
completion drain이다
(`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:91-180`).

## 실행 결과

모든 sample 실행은 `TMPDIR=/dev/shm/zlink-tmp-java`, 지정된 `ZLINK_LIBRARY_PATH`,
`ZLINK_JAVA_STREAM_TRACE=1`, `ZLINK_SAMPLE_KEEP_RUN_DIR=1`과
`flock -w7200 /tmp/zlink-jvm-gate.lock`을 사용했다. ZoneWorld는 자체 설정으로 message flow
`NORMAL`을 기록한다.

| 실행 | 결과 | 보존 로그 |
|---|---|---|
| prefix 1 | G1/G5/A1/A2 통과 | `bucketB-java-zoneworld-current-prefix1.log` 및 같은 이름의 role-log 디렉터리 |
| prefix 2 | G1/G5/A1 통과, A2 join timeout | `bucketB-java-zoneworld-current-prefix2.log` 및 role-log 디렉터리 |
| full 1 | A2 통과, A3 이후 여러 scenario 실패, 최종 marker withheld | `bucketB-java-zoneworld-current-full1.log` 및 role-log 디렉터리 |
| SUB poller 수정 prefix | G1/G5/A1 통과, A2 join timeout | `bucketB-java-zoneworld-fixed-prefix.log` |
| SUB 수정 검증 prefix | G1/G5/A1 통과, A2 join timeout; SUB 예외 0건 | `bucketB-java-zoneworld-final-prefix.log`, run dir `tmp.KS2bojRaXL` |

전체 실행은 끝까지 진행되어 `zoneworld=completed`가 `ZW-A3` 실패 때문에 withheld되었다.
따라서 요구된 full ZoneWorld 2회 green은 실행하지 못했다.

Runtime 후보를 수정한 상태에서 실행한
`ZLinkJavaSocketReceiveOwnerTest.subscriberReadinessDoesNotClaimAnUnsupportedCompletionQueue`는
통과했다. 최종 코드에는 해당 후보를 남기지 않았다. `:zlink-framework-core:test`는 1,212개 중
2개가 실패했다.

- `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`
- `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`

두 실패는 SUB poller 후보 test와 다른 M6A descriptor-fence 경로다. 첫 실제 실패 뒤 전체 gate를
반복하지 않았다.

## 변경과 BLOCKERS

- 최종 변경은 이 조사 요약뿐이다. runtime과 sample 코드는 원래 상태로 되돌렸으며 commit하지
  않았다.
- BLOCKER: trace는 A2의 service request/send 호출과 native admission completion 사이에서
  지연을 확인했지만, 그 안의 어느 단계가 누적 부하에 영향을 받는지는 구분하지 못했다.
  Java binding 수정은 이번 job에서 금지되어 있으며, Framework에 비동기 handoff를 추가한
  후보는 반복 prefix에서 같은 결과를 내지 못했다.
- BLOCKER: 반복 실행에서 actor가 entry node와 다른 node에 배치되었을 때 remote `JoinSpot`
  처리 결과가 `DATA_LOST`가 되는 별도 실패를 관찰했다. 그 뒤 `MoveMsg`는 entry spot에서
  `no_handler`로 폐기됐다. 이 경로가 최초 gateway 3.010 s 지연과 같은 원인이라는 증거는 없다
  (`/dev/shm/zlink-tmp-java/tmp.OMrZs40xdd/logs/zone-node-2.log:14568-14571`,
  `:15519-15523`).
- BLOCKER: `:zlink-framework-core:test`의 기존 M6A 실패 2건 때문에 runtime 전체 gate도 green이
  아니다.
