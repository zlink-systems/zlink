# Node routed receive cache와 단일 part 수신 결과

## 조건

- branch: `core-0.10.0-bindings-performance`
- Core: release `0.10.1`
- Node: `22.23.2`
- transport: tcp, clients: 100, duration: 2초, runs: 1
- message size: 64·256·1024·4096·65536·131072B
- C와 Node를 순서대로 실행했다. 병렬 측정은 하지 않았다.

## 적용한 binding hot path

routed receive는 `Received`를 재사용해도 addon이 routing-id Buffer를 매번 새로 만들었다.
addon은 직전 receive의 내부 Buffer와 새 routing-id bytes가 같으면 해당 Buffer를 재사용한다.
`Received` materializer도 같은 Buffer identity이면 중복 byte compare를 하지 않는다. public
`RoutingId`와 `Received` interface, ownership 규칙은 변경하지 않았다.

일반적인 one-part routed receive는 addon 안에서 매번 `std::vector<zlink_msg_t>`를 만들었다.
one-part일 때는 stack `zlink_msg_t`를 곧바로 JS owner로 옮기고, multipart만 기존 vector 경로를
사용하도록 변경했다. 이 경로는 ROUTER relay와 terminal reader 모두에서 사용된다.

terminal reader가 `data()`를 반복해서 읽는 경우에는 64B 이하 one-part만 recv 경계에서 managed
Buffer로 materialize한다. 그 외에는 native frame을 유지해 unread relay의 ownership transfer를
보존한다.

## `MULTI_ROUTER_ROUTER_SENDSEND / tcp` 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 174,024.5 | 79,642.5 | 45.77% |
| 256B | 181,784.0 | 68,463.0 | 37.66% |
| 1024B | 179,812.0 | 53,884.5 | 29.97% |
| 4096B | 166,703.0 | 50,389.0 | 30.23% |
| 65536B | 37,916.5 | 18,335.5 | 48.36% |
| 131072B | 20,606.0 | 10,116.0 | 49.09% |
| 산술평균 | - | - | 40.18% |

- C report: `/tmp/zlink-node22-rr-rid-cache-c/multi/report/perf_c_multi_linux_20260813_210535_node22-rr-rid-cache-c.txt`
- Node report: `/tmp/zlink-node22-rr-route-identity-final/multi/report/perf_node_multi_linux_20260813_211649_node22-rr-route-identity-final.txt`

## Node tcp 재측정 결과

| Pattern | Node / C 산술평균 | 판정 |
|---|---:|---|
| `MULTI_DEALER_DEALER` | 41.93% | 최소 기준 35% 통과, 중앙값 목표 60% 미달 |
| `MULTI_PUBSUB` | 40.07% | 최소 기준 35% 통과, 중앙값 목표 60% 미달 |
| `MULTI_ROUTER_ROUTER_SENDSEND` | 40.18% | 최소 기준 30% 통과, 중앙값 목표 60% 미달 |
| `MULTI_STREAM` | 59.69% | 최소 기준 30% 통과, 중앙값 목표 60% 미달 |

PUB/SUB 전체 run의 65536B case는 Core mutex `Invalid argument`으로 실패했다. 같은 조건의
단독 실행은 35,883.0 msg/s로 완료했고, 표에는 이 결과를 사용했다.

STREAM C report: `/tmp/zlink-node22-stream-final-c/multi/report/perf_c_multi_linux_20260813_212342_node22-stream-final-c.txt`
Node report: `/tmp/zlink-node22-stream-final/multi/report/perf_node_multi_linux_20260813_212432_node22-stream-final.txt`

## 검증

- Node 22 addon Release build 통과
- TypeScript build와 typecheck 통과
- routed receive·callback·multipart raw test 10개 통과
