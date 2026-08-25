# C++ Single — ROUTER_ROUTER_REQREP / tls (local Core 0.13.2)

## 결론

- 최종 상태: **미달**
- secure 5-run median paired 결과: throughput aggregate mean **71.43%**, latency median ratio **1.65x**
- request/reply 기준(throughput 85% 이상, latency 2.0x 이하)에서 latency는 통과했지만 throughput은 미달이다.

## 최종 측정

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_195158_cpp-router-router-reqrep-tls-local0132-baseline-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_195453_cpp-router-router-reqrep-tls-local0132-baseline-cpp-20260825.txt`
- Core source: local `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2` (0.13.2)

| Size | C msg/s | C++ msg/s | Throughput ratio | C latency ms | C++ latency ms | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 154,426.4 | 83,529.0 | 54.09% | 0.314 | 0.678 | 2.16x |
| 256 | 141,189.4 | 74,303.6 | 52.63% | 0.355 | 0.800 | 2.25x |
| 1,024 | 83,390.4 | 35,209.8 | 42.22% | 0.723 | 1.807 | 2.50x |
| 65,536 | 5,344.2 | 4,489.2 | 84.00% | 2.313 | 2.649 | 1.15x |
| 131,072 | 3,052.0 | 2,891.2 | 94.73% | 2.136 | 2.043 | 0.96x |
| 262,144 | 1,660.8 | 1,676.0 | 100.92% | 1.801 | 1.744 | 0.97x |

## 개선 pass와 계약 gate

1. 후보 A — initial exact transport target 대신 Core normal route selection 사용
   - **no-go**: pair id/generation을 생략하면 초기 선택 대상이 사라진 뒤 다른 대상로 failover될 수 있다.
     이는 exact-target, terminal/no-reroute, refused-submit ownership 공개 계약을 바꾼다.
   - 따라서 벤치마크 전에 제외했다.
2. 후보 B — async-only completion bridge 유지
   - 현재 source에 반영된 경로로, managed bridge의 mutex, condition variable, optional staging을 async 경로에서 제거한다.
   - 다른 transport의 before/after를 TLS A/B 수치로 오인하지 않는다. 이 TLS 결과는 현재 source의 최종 baseline이다.
   - 다음 contract suite를 재실행해 **5/5 통과**했다:
     `test_cpp_contract_request_reply`, `test_cpp_contract_exact_request_target`, `test_cpp_contract_message`, `test_cpp_contract_socket`, `test_cpp_contract_behavior`.

작은 메시지의 completion/dispatch 고정비용은 후속 프로파일링 대상이지만, 이 pass에서 public ownership·exactly-once·close/failure/cancellation·concurrency·callback context를 바꾸지 않는 독립 후보는 확인되지 않았다.
