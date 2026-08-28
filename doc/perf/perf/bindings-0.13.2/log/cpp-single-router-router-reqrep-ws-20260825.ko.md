# C++ Single ROUTER_ROUTER_REQREP / ws — local Core 0.13.2

- Core/runtime: local `0.13.2`, baseline revision `453dc26538`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_174829_cpp-router-router-reqrep-ws-local0132-baseline-c-20260825.txt`
- C++ before: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_175007_cpp-router-router-reqrep-ws-local0132-baseline-cpp-20260825.txt`
- C++ 후보 B after: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_180136_cpp-router-router-reqrep-ws-local0132-candidate-b-ownershipfix-20260825.txt`

| Pass | Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---|---:|---:|---|
| before | 27.67%, 29.84%, 30.11%, 85.14%, 84.13%, 84.27% | **56.86%** | **2.41x** | 미달 |
| A | — | — | — | no-go: exact-target contract |
| B | 31.00%, 32.61%, 33.55%, 86.02%, 91.03%, 85.14% | **59.98%** | **2.15x** | 개선 채택, 목표 미달 |

후보 A는 normal router request API로 단일 part의 dispatch 단계를 줄이는 방안이었다. 하지만 현재
binding은 monitor가 선택한 `transport_pair_id`와 generation을 Core에 그대로 전달해 exact target을
보장한다. 이를 생략하면 연결 변동 시 다른 peer로 failover할 수 있고 terminal/failure 의미가 바뀐다.
공개 계약을 바꾸지 않는다는 기준에 따라 benchmark 전 no-go로 확정했다.

후보 B는 `.async()` 전용 completion을 `async_operation_state_t`에 직접 연결했다. callback과 blocking
API는 기존 `managed_request_bridge_t`를 유지한다. 따라서 async hot path에서 shared_ptr control block,
mutex/optional staging, userdata의 한 단계 간접 참조를 제거하면서, callback context와 blocking wait의
책임 경계는 그대로 둔다. 이는 async 단일 소비자와 다중 소비자 bridge를 분리한 POSDDD 정리이며,
public message/routing-id ownership, exactly-once completion, close·timeout·cancellation·concurrency
semantics를 바꾸지 않는다.

## 최종 검증

- candidate B source에서 `test_cpp_contract_message`, `test_cpp_contract_socket`,
  `test_cpp_contract_request_reply`, `test_cpp_contract_behavior`가 4/4 통과했다.
- aggregate는 56.86%에서 59.98%로 +3.12%p, latency median은 2.41x에서 2.15x로 개선됐다.
  그러나 request/reply throughput 목표 85.00%와 latency 상한 2.0x에는 모두 미달하므로 최종 상태는
  `미달(59.98%)`다. 개선 source는 성능·POSDDD 이득과 contract 검증이 있으므로 유지한다.
