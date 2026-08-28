# C++ Single DEALER_DEALER / ipc — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `27214a31f6` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 paired 기준선 — 3 runs

`DEALER_DEALER / ipc / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_120918_cpp-dealer-dealer-ipc-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_120918_cpp-dealer-dealer-ipc-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_120934_cpp-dealer-dealer-ipc-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_120934_cpp-dealer-dealer-ipc-core0130-before-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,082,540.8 | 1,670,436.0 | 80.21% | 0.554 ms | 0.383 ms | 0.691x |
| 256 | 1,182,728.6 | 1,104,254.4 | 93.36% | 1.450 ms | 1.646 ms | 1.135x |
| 1024 | 608,962.4 | 546,656.2 | 89.77% | 0.811 ms | 0.901 ms | 1.111x |
| 65536 | 29,764.6 | 20,302.4 | 68.21% | 0.336 ms | 0.518 ms | 1.542x |
| 131072 | 18,074.2 | 13,247.4 | 73.29% | 0.336 ms | 0.472 ms | 1.405x |
| 262144 | 9,868.8 | 8,778.8 | 88.96% | 0.412 ms | 0.460 ms | 1.117x |

- throughput ratio 산술평균: **82.30%** — 기본 95%와 완화 90% 목표 모두 미달
- mean-latency ratio 산술평균: **1.167x** — C++ 2.0x 상한 통과

## 공개 경로 검토

IPC endpoint 생성, bind/`last_endpoint` 해석과 connection-ready 이외의 활성 구간은 transport
공통 DEALER public terminal이다. `message_t::from`의 payload ownership, pooled state, failure
restore, socket-close liveness와 outbound attempt serialization은 기존 IPC PAIR/PUBSUB 검토와
같이 계약상 제거할 수 없다. C API/private path, raw native message 재사용, public builder/ownership
변경, weak/mutex 제거, pool 확대·large-message pool은 모두 no-go다.

안전한 binding 변경 후보가 없고 처리량이 완화 목표보다 낮으므로 source 변경 없이
**DEALER_DEALER/ipc는 보류**다. final 5회는 목표 경계가 아니므로 요구되지 않는다.
