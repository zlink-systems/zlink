# C++ Single DEALER_ROUTER / inproc — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `a8d8394631` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |

## Smoke와 paired 기준선 — 3 runs

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_124220_cpp-dealer-router-inproc-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_124221_cpp-dealer-router-inproc-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_124230_cpp-dealer-router-inproc-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_124407_cpp-dealer-router-inproc-core0130-before-cpp-20260825.txt`
- paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

| Size | throughput ratio | latency ratio |
|------|-----------------:|--------------:|
| 64 | 86.00% | 1.991x |
| 256 | 84.97% | 1.210x |
| 1024 | 79.07% | 1.568x |
| 65536 | 19.50% | 3.500x |
| 131072 | 42.65% | 1.929x |
| 262144 | 82.72% | 1.120x |

- throughput ratio 산술평균: **65.82%** — routed one-way 목표 85% 미달
- mean-latency ratio 산술평균: **1.886x** — C++ 2.0x 상한 통과

## 공개 경로 검토

C와 C++ 모두 active sender에서 reusable caller payload를 stamp한 뒤 full payload를 message에
복사한다. C++ `message_t::from`은 public message ownership을 만들고 `send_routed`는 public
Dealer terminal을 수행한다. Router 수신의 `routing_id_t`와 `message_t`는 public 반환물이다.
native message/routing-id 재사용, C API/private path, message ownership/close 변경, weak/mutex
제거, large-message pool 증설은 계약 또는 측정 정책을 위반한다.

64/128KiB의 급락은 C와 C++ 모두 inproc/native Core 분기에 걸쳐 나타난다. 안전한 binding 변경
후보가 없고 목표보다 낮으므로 source 변경 없이 **DEALER_ROUTER/inproc는 보류**다. 목표 경계가
아니므로 final 5회는 요구되지 않는다.
