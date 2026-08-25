# C++ Single PAIR / tcp — local Core 0.13.2

## Manifest

- Core: local `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- Core revision: `dd4b246e8ae5e5d502bab31d76c90b2faaaa7afe`, `core_dirty=0`
- C/C++ runner: `--duration 5 --runs 5 --msg-sizes 64,256,1024,65536,131072,262144`
- shared options: one client, balanced auto-HWM, automatic HWM, default one I/O thread
- aggregate definition: six size-level throughput ratios의 산술평균; latency는 size-level
  mean-latency ratio의 중앙값

## Before — strict target 미달

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_145312_cpp-pair-tcp-local0132-final5-c-20260825.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_145550_cpp-pair-tcp-local0132-final5-cpp-20260825.txt`
- throughput ratio: `93.10%, 90.08%, 95.14%, 80.02%, 90.51%, 97.10%`
- throughput aggregate: **90.99%** (target 95.00% 미달)
- latency ratio median: **1.12x** (2.0x 상한 통과)

## Candidate A — bounded large-message reuse

`bindings/cpp/src/Runtime/Messaging/message.cpp`의 기존 pool을 128KiB–1MiB 범위에서
활성화했다. 상한은 기존 8MiB 그대로이며, Core가 message를 해제한 뒤에만 release callback이
pool에 반환한다. public API, send 성공 시 consume, 실패 시 ownership 복구, close/cancel,
mutex 기반 동시성 보호 및 callback context는 변경하지 않았다.

- Sol review가 지적한 static teardown UAF 위험을 보강했다. pool singleton을 프로세스 종료까지
  생존시켜 late Core release callback이 파괴된 mutex/vector에 접근하지 않는다. retained storage는
  기존 상한 8MiB를 넘지 않으며 OS가 process exit 때 회수한다.
- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_151518_cpp-pair-tcp-local0132-candidate-a-teardown-final5-c-20260825.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_151758_cpp-pair-tcp-local0132-candidate-a-teardown-final5-cpp-20260825.txt`
- throughput ratio: `94.91%, 100.44%, 97.84%, 85.87%, 95.56%, 91.62%`
- throughput aggregate: **94.37%** (before 대비 +3.38%p, strict target 미달)
- latency ratio median: **1.04x** (통과)
- contract: `test_cpp_contract_message`, `test_cpp_contract_socket`,
  `test_cpp_contract_behavior` 모두 통과.

## Candidate B — 폐기

같은 pool의 최소 크기만 64KiB로 낮춰 64KiB 병목을 겨냥했으나, 3회 탐색에서 64KiB가
timeout되어 report가 partial(25/30 result lines)이 됐다. 따라서 성능 수치와 무관하게
즉시 폐기하고 128KiB 하한으로 되돌렸다.

- partial report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_150618_cpp-pair-tcp-local0132-candidate-b-cpp-20260825.txt`

## 판정

Sol read-only review는 정상 수명에서 ownership/정확히 한 번 callback/fallback 의미가 유지됨을
확인했고, 위 static teardown 보강 전에는 UAF 위험이 있다고 판정했다. 보강 후
`test_cpp_contract_message`, `test_cpp_contract_socket`, `test_cpp_contract_behavior`가 모두
통과했다.

후보 A는 안전성 회귀 없이 aggregate를 개선해 유지한다. 그러나 95% strict target에는
도달하지 못했으므로 `PAIR / tcp` 최종 상태는 **미달(94.37%)**이다. 후보 B의 timeout은
재도입하지 않는다.
