# C++ Single DEALER_ROUTER_REQREP / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `a1687281a4fd95be3ad80071682667994953c456`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `166.74 Kops/s`; C++ `69.35 Kops/s` (41.59%).
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_183908_cpp-dealer-router-reqrep-wss-local0132-baseline-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_184223_cpp-dealer-router-reqrep-wss-local0132-baseline-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 43.96% | 2.53x |
| 256B | 39.93% | 2.67x |
| 1024B | 40.89% | 2.76x |
| 65536B | 89.44% | 1.11x |
| 131072B | 95.47% | 1.03x |
| 262144B | 96.42% | 1.01x |

## 개선 pass와 최종 판정

- 후보 A: 단일 part에서 normal DEALER request API를 사용해 exact transport-pair 선택을 생략하는 방안은
  initial target 부재 시의 terminal `not_connected`와 failover 의미를 바꾼다. 이전 public contract test
  (`test_dealer_request_without_initial_routed_target_is_terminal`)에서 확인된 계약 위반이므로 no-go다.
- 후보 B: async-only completion bridge는 callback/blocking bridge의 mutex·staging을 async hot path에서
  분리한 이미 채택된 source다. ownership, exactly-once, close/cancel/concurrency/callback-context를 보존하며
  이 기준선은 그 안전한 구현을 포함한다. B를 되돌리는 것은 성능·POSDDD 이득을 버리는 회귀이므로 하지 않는다.
- throughput aggregate는 **67.69%**로 request/reply 목표 85%에 미달하지만, latency median은 **1.82x**로
  2.0x gate를 통과한다. 최종 상태는 `미달(67.69%)`다.
