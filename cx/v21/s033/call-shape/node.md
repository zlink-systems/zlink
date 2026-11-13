# Node call-shape: L0

비교 범위: `zlink-node` raw target과 `ZLINK_LADDER_L0_SERVER=1`로 활성화한 L0 target. 이 L0 경로는 raw binding client의 multipart wire 형식을 사용한다. 정상 측정에서 `frameworkWire`는 기본 false이며, `ZLINK_LADDER_L0_FRAMEWORK_CLIENT` 또는 `ZLINK_CROSS_FRAMEWORK_WIRE` 설정을 켠 별도 wire 변형은 이 표에 포함하지 않는다.

| path step | binding call (file:line) | L-step call (file:line) | same? | if different: restored feature |
|---|---|---|---|---|
| Source send — legacy envelope + protobuf body, request/send multipart submit | `framework/bench/grpc/node/client/main.js:243-269` | 같은 raw transport submit; ladder L0 raw client `client/main.js:243-269` | yes | — |
| Target wait/receive — `DontWait` 반복, batch 상한까지 pump 후 `setImmediate` 재예약 | `framework/bench/grpc/node/zlink-raw-server/main.js:47-54,64-76,95-108` | `framework/bench/grpc/node/zlink-framework-server/main.js:59-72,95-105` | yes | — |
| Target per-message metrics — protobuf body decode 뒤 `metrics.record(body)` | `framework/bench/grpc/node/zlink-raw-server/main.js:77-82,108-113`; `shared/bench-server-metrics.js:32-42` | `framework/bench/grpc/node/zlink-framework-server/main.js:73-86`; 같은 `shared/bench-server-metrics.js:32-42` | yes | — |
| Target payload decode — 마지막 part의 protobuf body decode | `framework/bench/grpc/node/zlink-raw-server/main.js:56-59,77-80,108-111` | `framework/bench/grpc/node/zlink-framework-server/main.js:73-78,82` | yes | — |
| Target reply build/submit — 새 response envelope와 protobuf body 두 부분을 reply/send로 제출 | `framework/bench/grpc/node/zlink-raw-server/main.js:82-86` | `framework/bench/grpc/node/zlink-framework-server/main.js:78-83` | yes | — |
| Source completion/reply receive — binding submission reply Promise에서 마지막 protobuf 부분 decode, completion poller 대기 | `framework/bench/grpc/node/client/main.js:202-240,243-260,263-285` | 같은 L0 raw binding transport/Promise path `client/main.js:202-240,243-260,263-285` | yes | — |
| Source per-message metrics — 공통 stream counter update와 completion 처리 | `framework/bench/grpc/node/client/bench-core.js:350-405` | 같은 `bench-core.js` submit/complete path | yes | — |

Node raw와 L0 target 양쪽 모두 `DontWait` 수신과 동일한 batch pump 패턴을 사용한다. 여기서는 call shape만 비교했다.
