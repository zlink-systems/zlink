# C++ Single DEALER_DEALER / tls — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `a78cc3b477` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최초 paired 기준선

`DEALER_DEALER / tls / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_115630_cpp-dealer-dealer-tls-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_115630_cpp-dealer-dealer-tls-core0130-smoke-cpp-20260825.txt`
- 최초 C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_115641_cpp-dealer-dealer-tls-core0130-before-c-20260825.txt`
- 최초 C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_115641_cpp-dealer-dealer-tls-core0130-before-cpp-20260825.txt`

최초 3회 paired 결과는 throughput 산술평균 **90.85%**, mean-latency 산술평균 **1.045x**였다.
Secure transport 규칙에 따라 초기 결과와 무관하게 동일 manifest C→C++ final 5회를 실행했다.

## 공개 경로 검토

TLS 인증서 설정, TLS record 암호화와 write/receive queue는 Core 0.13.0 소유다. C++ DEALER
활성 경로의 `message_t::from`→pooled public operation state→동기 `submit()`은 PAIR/PUBSUB TLS와
같은 ownership/liveness/serialization 경계를 사용한다. C API/private path, native message 재사용,
public builder·실패 ownership·close 계약 변경, weak/mutex 제거, pool 확대 또는 large-message pool은
계약 또는 측정 경로를 바꾸므로 no-go다. Core TLS 경계를 제외하고 이 pattern에만 적용 가능한
안전한 binding 후보는 없다.

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_115834_cpp-dealer-dealer-tls-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_120113_cpp-dealer-dealer-tls-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,443,484.2 | 2,147,796.2 | 87.90% | 1.717 ms | 0.662 ms | 0.386x |
| 256 | 924,460.8 | 855,051.4 | 92.49% | 1.827 ms | 1.976 ms | 1.082x |
| 1024 | 293,527.2 | 249,199.8 | 84.90% | 1.684 ms | 1.971 ms | 1.170x |
| 65536 | 10,741.0 | 9,118.2 | 84.89% | 0.909 ms | 1.089 ms | 1.198x |
| 131072 | 6,373.0 | 5,379.8 | 84.42% | 0.922 ms | 1.102 ms | 1.195x |
| 262144 | 3,474.4 | 2,840.2 | 81.75% | 1.140 ms | 1.397 ms | 1.225x |

- throughput ratio 산술평균: **86.06%** — 기본 95%와 완화 90% 목표 모두 미달
- mean-latency ratio 산술평균: **1.043x** — C++ 2.0x 상한 통과

안전한 binding 변경 후보가 없고 final 5회 처리량이 완화 목표보다 낮으므로 source 변경 없이
**DEALER_DEALER/tls는 보류**다. TLS path profile은 Core/native 진단 과제로 분리한다.
