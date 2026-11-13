# L2 reply envelope 경로 대조

이 작업은 6–7,14–15 CPU에서 20초 이내의 진단 profile만 실행한다. 이전 정식 측정의 throughput 판정을 다시 내리지 않는다. Java scratch=e8721a0776, Node scratch=2bef18aa94. 아래 상대 경로는 `framework/bench/grpc/<language>/` 기준이다.

| 언어·단계 | binding 경로 | L1 → L2 경로 | 차이와 소유 기능 |
|---|---|---|---|
| Java source submit | `client/.../RawStack.java:73-98`, request → header/body → timeout → submit | `RawStack.java:100-126`, requestWithEnvelope → production encode → 같은 submit | L2 요청 header encode, header Message와 multipart view. body protobuf encode 동일 |
| Java target receive | `zlink-raw-server/.../ZLinkRawBenchServer.java:94-109`, blocking recv → body decode → metrics | `zlink-framework-server/.../ZLinkFrameworkBenchServer.java:129-185,201-227`, permit → recv → body decode → metrics. L2 `:208-220` header parse 추가 | L1 permit, L2 header decode. 기존 ladder는 최초 blocking 뒤 DONT_WAIT drain, raw는 각 recv blocking. 이 차이는 L1/L2 공통이며 binding 대비 수치 판정에는 사용하지 않음 |
| Java target reply | raw server `:115-127`, 새 header Message + 새 protobuf body → reply submit | framework server `:228-259`, L1 raw header, L2 `reply(requestHeader)` → production envelope encode → 같은 submit | L2 reply header encode. body 재사용 없음. encode 결과 소유권 정리만 추가 |
| Java source completion | `RawStack.java:270-291`, reply callback → protobuf/header 검증 → parts close | `RawStack.java:293-318`, reply callback → production header decode → 동일 검증/close | L2 reply header parse. callback·poller·admission·stream 진행 방식 동일 |
| Node source submit | `client/main.js:253-274`, request → raw header/body → timeout → submit | `client/main.js:253-284`, L2 `shared/raw-wire.js:95-111` production header encode | L2 요청 encode. submit/reply Promise 체인 동일 |
| Node target receive | `zlink-raw-server/main.js:47-53,64-81`, DontWait recv → body decode → metrics | `zlink-framework-server/main.js:166-205`, permit await → 동일 DontWait recv/body decode/metrics. L2 `:200` header parse | L1 permit await, L2 header decode. batch/setImmediate pump 동일 |
| Node target reply | raw server `:82-86`, 새 protobuf body → raw header/body → reply submit | framework server `:206-212`, L2 `shared/raw-wire.js:114-116` production reply encoder → 같은 submit | L2 reply header. protobuf Buffer 소유권 전달, 추가 body copy 없음 |
| Node source completion | `client/main.js:270-284`, reply.then → body decode → parts close | 동일 callback에서 L2 header decode/kind 검사 추가 | L2 reply header parse. 추가 Promise/await 없음 |

L1/L2 공통: Java binding completion poller 하나, Node binding completion poller 하나. 요청별 새로운 poller·timer·재시도 없음. L2가 더하는 것은 request/reply header encode와 decode, 그 결과 객체 및 header bytes다. profile sampler가 CPU/alloc/wall 또는 event-loop 관찰만 추가한다.
