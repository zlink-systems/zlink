# Node L1/L2 call shape

bench는 git archive 2bef18aa94, production은 n2net branch. 상대 경로는 framework/bench/grpc/node/이다.

| 단계 | binding | L1/L2 | 차이 소유 |
|---|---|---|---|
| source send | client/main.js:253-274 raw header+new protobuf+submit | client/main.js:253-284; shared/raw-wire.js:89-111 production encode | L2 요청 header, 나머지 동일 |
| target receive | zlink-raw-server/main.js:47-53,64-81 DontWait→drain→metrics | zlink-framework-server/main.js:166-205 같은 pump, permit await→recv, :200 header parse | L1 permit, L2 parse |
| target reply | zlink-raw-server/main.js:82-86 new protobuf+header→submit | zlink-framework-server/main.js:206-212; shared/raw-wire.js:114-116 new protobuf+production reply encode→submit | L2 reply envelope |
| source completion | client/main.js:270-284 reply.then→body decode→close | 동일 + L2 header decode/kind 검사 | L2 reply parse |

L1/L2의 wait·queue·atomics는 동일, L2는 request/reply header encode/decode만 추가한다. body protobuf 생성은 양쪽 매번 새로 하고 reply에 request parts를 재사용하지 않는다. per-message waits: target permit await 1, DontWait pump/setImmediate 동일, source binding completion Promise 동일. L2 header allocation/copy만 report.md에 별도 계수. source/target 모두 CPU 6–7,14–15; 정식 throughput 측정이 아닌 진단 profile이다.
