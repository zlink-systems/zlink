# C++ Single DEALER_DEALER / ws — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `3bcf3c02f5` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최초 paired 기준선

`DEALER_DEALER / ws / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_113832_cpp-dealer-dealer-ws-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_113832_cpp-dealer-dealer-ws-core0130-smoke-cpp-20260825.txt`
- 최초 C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_113845_cpp-dealer-dealer-ws-core0130-before-c-20260825.txt`
- 최초 C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_113845_cpp-dealer-dealer-ws-core0130-before-cpp-20260825.txt`

최초 3회 paired 결과는 throughput 산술평균 **92.92%**, mean-latency 산술평균 **1.160x**였다.
기본 목표 95% 경계 안이므로 source 변경 전에 동일 manifest C→C++ final 5회를 사전 결정해 실행했다.

## 공개 경로 검토

WebSocket handshake, framing, write queue와 poll readiness는 Core 0.13.0이 소유하며,
DEALER의 C++ 활성 경로는 `message_t::from`→pooled public send builder→동기 `submit()`이다.
payload 소유권 복원, socket close liveness, outbound attempt 직렬화는 transport와 무관한
공개 terminal 계약이다. raw C/private path, native message 재사용, builder/ownership/close
규칙 변경, weak/mutex 제거, pool 확대 또는 large-message pool은 측정 경로 또는 공개 계약을
바꾸므로 no-go다. public 계약을 유지한 채 해당 비용을 더 제거할 후보는 확인되지 않았다.

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_114053_cpp-dealer-dealer-ws-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_114334_cpp-dealer-dealer-ws-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,545,619.2 | 2,046,316.4 | 80.39% | 36.332 ms | 48.707 ms | 1.341x |
| 256 | 1,168,338.6 | 1,186,510.8 | 101.56% | 29.473 ms | 23.337 ms | 0.792x |
| 1024 | 436,168.0 | 427,665.4 | 98.05% | 19.046 ms | 18.067 ms | 0.949x |
| 65536 | 21,988.0 | 20,085.2 | 91.35% | 6.505 ms | 6.180 ms | 0.950x |
| 131072 | 14,762.0 | 13,572.6 | 91.94% | 0.402 ms | 0.442 ms | 1.100x |
| 262144 | 8,963.8 | 8,922.4 | 99.54% | 0.461 ms | 0.467 ms | 1.013x |

- throughput ratio 산술평균: **93.80%** — 기본 95%에는 미달, C++ 단순 one-way 완화 목표 90% 통과
- mean-latency ratio 산술평균: **1.024x** — C++ 2.0x 상한 통과

PAIR/ws 및 PUBSUB/ws도 동일 release runtime에서 완화 목표 90%로 통과한 이력이 있으며,
DEALER_DEALER/ws final 5회도 두 gate를 만족한다. 따라서 source 변경 없이
**DEALER_DEALER/ws는 완화 목표 90%로 통과**다.
