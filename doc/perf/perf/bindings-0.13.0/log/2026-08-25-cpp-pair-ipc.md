# C++ Single PAIR / ipc — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `1666fd3245` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 경계 재측정

`PAIR / ipc / 64B`, duration 1초, runs 1 smoke에서 C와 C++ 모두 `status: complete`였다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_101804_cpp-pair-ipc-core0130-smoke-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_101810_cpp-pair-ipc-core0130-smoke-cpp-20260825.txt`

최초 paired 3회 결과는 throughput 산술평균 90.14%, mean-latency 산술평균 1.155x였다.
완화 목표 90%보다 0.14%p만 높아 계획의 경계 셀 규칙에 따라 동일 manifest로 C→C++ 순서의
최종 5회 paired 측정을 수행했다.

- 최초 C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_101826_cpp-pair-ipc-core0130-before-c-20260825.txt`
- 최초 C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_102008_cpp-pair-ipc-core0130-before-cpp-20260825.txt`

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_102314_cpp-pair-ipc-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_102619_cpp-pair-ipc-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,533,677.0 | 2,068,866.0 | 81.65% | 0.185 ms | 0.185 ms | 1.000x |
| 256 | 1,606,180.8 | 1,464,393.2 | 91.17% | 1.055 ms | 1.173 ms | 1.112x |
| 1024 | 702,217.2 | 671,202.6 | 95.58% | 0.765 ms | 0.718 ms | 0.939x |
| 65536 | 32,211.2 | 26,660.6 | 82.77% | 0.300 ms | 0.393 ms | 1.310x |
| 131072 | 23,074.8 | 17,388.8 | 75.36% | 0.265 ms | 0.378 ms | 1.426x |
| 262144 | 14,311.8 | 11,743.4 | 82.05% | 0.281 ms | 0.341 ms | 1.214x |

- throughput ratio 산술평균: **84.77%** — 기본 95%와 완화 90% 목표 모두 미달
- mean-latency ratio 산술평균: **1.167x** — C++ 2.0x 상한 통과

## 자체 및 Sol read-only 검토

IPC 전용 코드는 endpoint 생성, bind, `last_endpoint` 확인과 연결 준비만 담당하고, 측정
loop에는 transport 분기가 없다. library active path도 다른 PAIR transport와 같은
`zlink_send_part`/`zlink_recv_part` submit/receive 경로다. Unix-domain IPC framing과 큐
동작은 Core 0.13.0 소유이므로 binding에서 제거할 IPC 전용 비용이 확인되지 않았다.

다음 후보는 public 경로 또는 수명/직렬화 계약을 깨므로 채택하지 않는다.

- C API 직접 호출 또는 private perf path
- public builder/API 변경
- large-message pool 활성화
- submit mutex 또는 weak-liveness 제거

자체 hot-path 검토와 Sol의 독립 read-only pass 후에도 계약 안전한 후보가 없으며, 최종 5회
측정에서 완화 목표에 미달했다. 따라서 source 변경 없이 **PAIR/ipc는 보류**다.
