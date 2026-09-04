# Bucket B — Java sample timeout cluster 조사 결과

## 결론

DeliveryDispatch의 `CompletionException: TimeoutException`은 D-B85 REQUEST backpressure나
reciprocal ROUTER handover가 원인이 아니다. Dispatch가 courier에게 제안을 보내기 전에 900 ms
응답 deadline을 시작하고, 그 사이 먼저 `DeliveryStatusChangedReq`의 완료를 기다리는 sample
순서 오류다. 기존 실패에서는 이 선행 request가 2.653 s 뒤 완료되어 courier가 제안을 받기
전에 attempt 1이 만료되었다. 뒤늦게 도착한 정상 결정은 stale attempt로 버렸고, attempt 2도
같은 순서로 만료되어 client stream sequence가 끝나지 않았다.

분류는 **sample**이다. 이번 job에서는 한 원인만 확정했다. GameQuest와 ZoneWorld는 같은 native
REQUEST 결과를 확인했지만 증상이 서로 달라 별도 후보로 남겼다.

요약 작성 시 workspace HEAD는 `2e3b1b47e43c2fcbe0eef7e9481a6af0d11ed16a`이고 branch는
`main`이다. sample이 실제로 읽은
`.artifacts/wsl/maven/systems/zlink/zlink/0.17.0/zlink-0.17.0.jar`의 SHA-256은
`6687fe2c1dce10e01d85bb12dbd313d203736efe106b0d8ebbd0007ea43e0af5`다.

## Native REQUEST 결과

Core의 `zlink_request_part`를 감싼 임시 shared-library probe로 submit result, `errno`,
completion ID와 socket 종류를 기록했다. probe 소스와 바이너리는 조사 후 삭제했고 보존한 role
log에 결과가 남아 있다.

DeliveryDispatch의 첫 정상 배차에서 문제의 선행 request는 다음 결과를 반환했다.

```text
DeliveryStatusChangedReq sent
ZLINK_REQUEST_PROBE pid=26960 result=0 errno=0 completion_id=2 flags=1 timeout_ms=30000 target=dealer
DeliveryStatusChangedReq reply_received
```

근거는 `zlink-work/c016/logs/bucketB-java-delivery-run2/dispatch.log:65-67`이다. `result=0`은
`SubmitResult.OK`이며 유효한 completion ID도 받았다. 해당 실행의 기능 경로에서 관찰한 REQUEST
24건은 모두 `result=0`이었다. `courier-session` 종료 중에만 ROUTER REQUEST 4건이
`result=2`, `errno=113`, `completion_id=0`을 반환했다. 이는 `SubmitResult.NOT_CONNECTED`이며
이미 알려진 `TEARDOWN_FAILED` 경로다
(`zlink-work/c016/logs/bucketB-java-delivery-run2/courier-session.log:158-161`).
여기서 `errno=113`은 별도 `errno` 필드다. submit result `1`인 `BACKPRESSURED`와 혼동하지
않았다.

`BACKPRESSURED`인 `result=1`은 DeliveryDispatch, GameQuest, ZoneWorld probe 실행에서 한 건도
관찰되지 않았다. 따라서 first stalled request가 wait token만 남기고 재제출되지 않았다는 E
가설은 이 실패를 설명하지 못한다. 문제의 request가 DEALER에서 `OK`로 접수되고 server에서
`received → admitted → dispatched → replied`를 모두 거쳤으므로 reciprocal ROUTER handover
가설도 맞지 않는다.

## DeliveryDispatch 원인

### 실패 순서

기존 gate log의 동일 flow `01a06d38-7e48-76cf-9a0d-5eceeed17523`은 다음 순서를 보여 준다.

| 시각 | 관찰 | 근거 |
|---|---|---|
| 01:20:14.028 | Dispatch가 `Assigned` 상태 request를 보냄 | `gate-samples-java-DeliveryDispatch.log:234` |
| reply 전 | sweeper가 attempt 1을 만료 처리 | 같은 log `:235` |
| 01:20:16.681 | 위 request의 reply가 2.653 s 뒤 도착 | 같은 log `:237` |
| 직후 | courier A의 정상 결정이 도착했으나 attempt 1을 stale로 폐기 | 같은 log `:239-242` |
| 이후 | attempt 2 만료, 후보 소진으로 `Failed` | 같은 log `:245-247` |

Tracking 쪽 flow도 request가 01:20:14.033에 들어와 dispatch되었고 01:20:16.639에 reply를
보냈음을 기록한다(`gate-samples-java-DeliveryDispatch.log:267-270`). 요청은 유실되지 않았다.

### 코드 경로와 계약

`DispatchWorker.dispatch()`는 먼저 `offers.offer(...)`를 호출한 다음 `publishStatus(...)`의
완료를 기다리고 courier actor send를 제출한다
(`framework/languages/java/samples/java/DeliveryDispatch/Server/Dispatch/src/main/java/systems/zlink/samples/deliverydispatch/server/dispatch/DispatchWorker.java:36-40`).
재배차도 같은 순서다(`DispatchWorker.java:80-83`).

`DeliveryOfferStore.offer()`는 호출 즉시 `Instant.now().plus(timeout)`으로 deadline을 기록한다
(`framework/languages/java/samples/java/DeliveryDispatch/Server/Dispatch/src/main/java/systems/zlink/samples/deliverydispatch/server/dispatch/DeliveryOfferStore.java:21-30`).
timeout은 900 ms다
(`framework/languages/java/samples/java/DeliveryDispatch/Server/Configuration/src/main/java/systems/zlink/samples/deliverydispatch/server/configuration/SampleTimings.java:11`).

sample 계약은 courier A가 `OfferDeliveryMsg`를 **받은 뒤** deadline 안에 응답하지 않을 때
attempt를 만료하도록 정한다
(`framework/doc/framework/common/sample/deliverydispatch/README.ko.md:292-296`, §7.2).
현재 구현은 courier가 제안을 받기 전의 status publish 지연까지 응답 시간에 포함하므로 이 계약과
다르다.

최소 수정 방향은 첫 배차와 재배차 모두 선행 status publish가 끝난 뒤, courier actor send와
인접한 지점에서 offer row와 deadline을 시작하는 것이다. 회귀 검증은 status publish를 900 ms
넘게 지연해도 courier send 전에는 attempt가 만료되지 않고, send 이후 응답 시간만 deadline에
포함되는지 확인해야 한다. 이번 job은 framework 원인일 때만 구현과
`zlink-framework-core` regression test를 추가하도록 한 범위이므로 sample 코드는 수정하지
않았다. assertion도 변경하지 않았다.

## 다른 sample의 동일 원인 여부

| sample | 재현 결과 | native REQUEST | 판단 |
|---|---|---|---|
| GameQuest | 2회 모두 `Ensure failed` | probe 실행의 ROUTER REQUEST 16건 모두 `result=0` | DeliveryDispatch와 다른 후보. client evidence의 유일한 실패는 `failure:unexpected:active-binding:player-alice`이므로 session unbind/cleanup 경계를 별도 조사해야 한다. |
| ZoneWorld | `ZW-A2` 단독 통과, 축소 prefix `ZW-G1,ZW-G5,ZW-A1` 뒤 `ZW-A2` 실패 | 단독 실행 11건, prefix 실행 22건 모두 `result=0` | DeliveryDispatch와 다른 후보. 누적 상태/부하가 있는 경우 gateway가 A2 `JoinWorldMsg`를 받은 뒤 actor send 완료까지 3.010 s 걸려 client의 3 s 대기를 넘었다. |

GameQuest의 전체 evidence는
`zlink-work/c016/logs/bucketB-java-gamequest-probe.log:455-456`에 있다. 진단용 client 출력은
로그를 보존한 뒤 제거했다.

ZoneWorld prefix 실행에서 gateway는 A2 join을 01:43:57.689에 받아 dispatch했고
01:44:00.699에 actor send를 완료했다
(`zlink-work/c016/logs/bucketB-java-zoneworld-prefix/gateway.log:89-121`). client는
`ScenarioSupport.java:109`에서 먼저 3 s timeout에 도달했다
(`zlink-work/c016/logs/bucketB-java-zoneworld-prefix.log:61-75`). 단독 A2는 통과했으므로 원래
full gate의 `moveTo` timeout과 함께 누적 부하/상태 경계를 별도 원인으로 조사해야 한다.

## 재실행 결과

| 실행 | trace/probe | 결과 |
|---|---|---|
| DeliveryDispatch 1 | message flow + `ZLINK_JAVA_STREAM_TRACE=1` | 기능 marker 3개 모두 통과. 전체 exit는 알려진 `courier-session` `TEARDOWN_FAILED` 때문에 1 |
| DeliveryDispatch 2 | message flow + stream trace + native probe | 기능 marker 3개 모두 통과. 전체 exit는 같은 teardown 실패 때문에 1. 기능 REQUEST 24건 모두 `OK` |
| GameQuest 1 | message flow + stream trace | `GameQuestClientScenario.java:215`의 `Ensure failed` 재현 |
| GameQuest 2 | message flow + stream trace + native probe + 임시 evidence 출력 | 같은 실패 재현. ROUTER REQUEST 16건 모두 `OK` |
| ZoneWorld `ZW-A2` | message flow + stream trace + native probe | 통과. REQUEST 11건 모두 `OK` |
| ZoneWorld prefix | message flow + stream trace + native probe | G1/G5/A1 통과, A2 join timeout. REQUEST 22건 모두 `OK` |

보존한 aggregate log는 다음과 같다.

- `zlink-work/c016/logs/bucketB-java-delivery-run1.log`
- `zlink-work/c016/logs/bucketB-java-delivery-run2.log`
- `zlink-work/c016/logs/bucketB-java-gamequest.log`
- `zlink-work/c016/logs/bucketB-java-gamequest-probe.log`
- `zlink-work/c016/logs/bucketB-java-zoneworld-a2.log`
- `zlink-work/c016/logs/bucketB-java-zoneworld-prefix.log`

각 실행의 role log는 확장자를 뺀 같은 이름의 디렉터리에 보존했다.

## 실행 명령

모든 sample 실행은 다음 환경과 JVM gate lock을 사용했다. native probe 실행에서는
`ZLINK_LIBRARY_PATH`만 임시 probe `.so` 파일로 바꾸었다. probe는 실제
`.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so`에 연결했다.

```bash
export TMPDIR=/dev/shm/zlink-tmp-java
export ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so
export ZLINK_JAVA_STREAM_TRACE=1

(cd framework/languages/java/samples/java/DeliveryDispatch && \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./run_sample.sh)
(cd framework/languages/java/samples/java/GameQuest && \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./run_sample.sh)
(cd framework/languages/java/samples/java/ZoneWorld && \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./run_sample.sh ZW-A2)
(cd framework/languages/java/samples/java/ZoneWorld && \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./run_sample.sh ZW-G1,ZW-G2-rid,ZW-G5,ZW-A1,ZW-A2)
```

마지막 selector의 `ZW-G2-rid`는 standalone scenario ID가 아니어서 실제로 실행된 축소 prefix는
G1/G5/A1/A2다. 원래 full gate의 A2 `moveTo` 실패와 비교할 별도 누적-state 재현으로만 사용했다.
artifact 확인에는 다음 명령을 사용했다.

```bash
sha256sum .artifacts/wsl/maven/systems/zlink/zlink/0.17.0/zlink-0.17.0.jar
javap -classpath .artifacts/wsl/maven/systems/zlink/zlink/0.17.0/zlink-0.17.0.jar \
  -c -p systems.zlink.runtime.sockets.CompletionOwner
```

## 변경과 BLOCKERS

- 변경: 이 조사 요약만 추가했다. framework, sample, Core, binding 코드는 변경하지 않았다.
- test: framework 변경이 없으므로 `zlink-framework-core` regression test는 추가하거나 실행하지
  않았다. 위 process sample 재실행으로 원인과 비동일 후보를 확인했다.
- BLOCKER: sample이 읽은 Maven artifact는 2026-09-05 00:38 생성본이다. workspace source의
  D-B85 Java REQUEST 수정 commit `a06260f50795916ef91cc0bd30b6c6a8f60e7903`
  (2026-09-05 00:59)보다 오래되었고, `javap` 결과도 `BACKPRESSURED`에서 WRITABLE을 기다려
  재제출하는 경로가 없는 이전 `CompletionOwner.submitRequest()`임을 확인했다. Core와 local
  package를 다시 만들지 말라는 제약 때문에 새 binding artifact의 sample gate는 확인하지
  않았다. 이 제약은 DeliveryDispatch의 확정 원인에는 영향을 주지 않는다. 문제의 submit은
  이전 artifact에서도 `OK`였기 때문이다.
- DeliveryDispatch sample 수정에는 차단 요인이 없다. GameQuest session cleanup과 ZoneWorld
  누적 부하/상태는 이번 job의 한 원인 범위를 벗어나므로 후속 조사 대상이다.
