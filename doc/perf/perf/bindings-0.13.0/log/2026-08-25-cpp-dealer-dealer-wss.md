# C++ Single DEALER_DEALER / wss — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `cfdd941060` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최초 paired 기준선

`DEALER_DEALER / wss / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_114740_cpp-dealer-dealer-wss-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_114740_cpp-dealer-dealer-wss-core0130-smoke-cpp-20260825.txt`
- 최초 C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_114755_cpp-dealer-dealer-wss-core0130-before-c-20260825.txt`
- 최초 C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_114755_cpp-dealer-dealer-wss-core0130-before-cpp-20260825.txt`

최초 3회 paired 결과는 throughput 산술평균 **97.46%**, mean-latency 산술평균 **1.030x**였다.
Secure transport 규칙에 따라 초기 결과와 무관하게 동일 manifest C→C++ final 5회를 실행했다.

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_114948_cpp-dealer-dealer-wss-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_115227_cpp-dealer-dealer-wss-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,960,936.6 | 2,174,178.4 | 110.87% | 48.803 ms | 46.606 ms | 0.955x |
| 256 | 745,333.2 | 758,506.2 | 101.77% | 44.763 ms | 39.869 ms | 0.891x |
| 1024 | 212,568.2 | 231,736.8 | 109.02% | 35.082 ms | 34.035 ms | 0.970x |
| 65536 | 8,154.6 | 8,668.2 | 106.30% | 16.920 ms | 14.907 ms | 0.881x |
| 131072 | 4,733.8 | 5,303.4 | 112.03% | 12.957 ms | 11.456 ms | 0.884x |
| 262144 | 2,597.8 | 2,684.6 | 103.34% | 12.471 ms | 12.087 ms | 0.969x |

- throughput ratio 산술평균: **107.22%** — C++ 단순 one-way 기본 목표 95% 통과
- mean-latency ratio 산술평균: **0.925x** — C++ 2.0x 상한 통과

공개 send terminal의 ownership, close 및 serialization 계약을 유지한 상태로 final 5회 두 gate를
통과했으므로 source 변경 없이 **DEALER_DEALER/wss는 기본 목표 95%로 통과**다.
