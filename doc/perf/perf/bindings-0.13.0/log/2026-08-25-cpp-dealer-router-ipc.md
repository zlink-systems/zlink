# C++ Single DEALER_ROUTER / ipc — Core 0.13.0

## Manifest and final paired judgement

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `35003dd355` |
| source / Core runtime | `0.13.2` / `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` |
| Core tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |

- smoke C/C++: `perf_{c,cpp}_single_linux_20260825_12470{8,9}_cpp-dealer-router-ipc-core0130-smoke-{c,cpp}-20260825.txt`
- 3회 기준선 C/C++: `perf_c_single_linux_20260825_124728_cpp-dealer-router-ipc-core0130-before-c-20260825.txt`; `perf_cpp_single_linux_20260825_124906_cpp-dealer-router-ipc-core0130-before-cpp-20260825.txt`
- 기준선 aggregate throughput 85.73%, latency 1.225x로 목표 경계여서 C→C++ 5회 중앙값을 최종 판정에 사용했다.
- final C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_125050_cpp-dealer-router-ipc-core0130-final5-c-20260825.txt`
- final C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_125330_cpp-dealer-router-ipc-core0130-final5-cpp-20260825.txt`
- 두 final report 모두 `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM 및 six sizes 일치.

| Size | throughput ratio | latency ratio |
|------|-----------------:|--------------:|
| 64 | 84.27% | 1.074x |
| 256 | 94.62% | 1.157x |
| 1024 | 96.43% | 1.053x |
| 65536 | 80.86% | 1.318x |
| 131072 | 87.11% | 1.174x |
| 262144 | 90.79% | 1.098x |

- throughput ratio 산술평균: **89.01%** — routed one-way 목표 85% 통과
- mean-latency ratio 산술평균: **1.146x** — C++ 2.0x 상한 통과

IPC endpoint setup은 활성 구간 밖이다. 활성 구간의 public `send_routed`와 Router
`recv(routing_id_t&, message_t&, dontwait)`는 routing-id/message ownership, poller readiness,
failure/close/serialization 계약을 보존해야 한다. C API/private path, native object 재사용,
ownership 변경, weak/mutex 제거, large-message pool 증설은 no-go다. 제거 가능한 binding 비용
후보 없이 목표를 통과했으므로 source 변경 없이 **DEALER_ROUTER/ipc는 통과**로 확정한다.
