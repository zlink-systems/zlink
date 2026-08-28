# C++ Single PUBSUB / ws — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `a7edf4cf79` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최초 paired 기준선

`PUBSUB / ws / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_103219_cpp-pubsub-ws-core0130-smoke-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_103219_cpp-pubsub-ws-core0130-smoke-cpp-20260825.txt`

최초 C→C++ paired 3회 결과는 throughput 산술평균 **88.08%**, mean-latency 산술평균
**1.092x**였다.

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_103230_cpp-pubsub-ws-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_103429_cpp-pubsub-ws-core0130-before-cpp-20260825.txt`

88.08%는 완화 목표 90%의 ±5%p 경계에 있어, 결과를 선택하지 않도록 source 변경 전에
동일 manifest의 C→C++ 5회 final 측정을 사전 결정하고 실행했다. 3회 결과는 최종 통과 근거로
덮어쓰지 않고 이 기록에 보존한다.

## 자체 및 Sol read-only 검토

WS는 endpoint setup에만 관여하고 활성 PUBSUB loop는 transport 공통이다. publish는
embedded-NUL 검증, pooled operation state, callback liveness, 단일 part direct publish를
이미 사용한다. `bench` topic은 SSO 범위라 topic copy도 heap allocation이 아니다.

subscribe는 이전 output의 ownership을 먼저 정리하고 native guard로 성공 시에만 새
`topic_message_t`를 commit하며, single part는 lazy storage로 vector materialization을 피한다.
topic 값은 public `topic_message_t::topic()` 계약의 일부라 제거할 수 없다.

다음 후보는 no-go다.

- validation 생략 또는 pointer/hash topic cache: mutable `std::string` 및 embedded-NUL 오류 계약 훼손 또는 비용 근거 없음
- `subscribe_part`로 perf harness 전환, C API/private fast path: 다른 public 경로 측정
- weak/mutex 제거: builder 생존성·close-vs-submit 직렬화 회귀
- pool 확대 또는 large-message pool: 기존 no-go, 활성 hot path 이득 근거 없음

자체와 Sol 독립 검토 모두 source 변경 후보가 없다고 결론냈다.

## 최종 paired 기준선 — 5 runs

- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_103856_cpp-pubsub-ws-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_104213_cpp-pubsub-ws-core0130-final5-cpp-20260825.txt`
- 두 report `status: complete`, 30/30 result lines
- Core runtime/revision/tag, Effective Options, auto-HWM과 six message sizes가 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,248,302.4 | 1,097,954.0 | 87.96% | 60.559 ms | 70.706 ms | 1.168x |
| 256 | 869,730.6 | 810,951.2 | 93.24% | 33.915 ms | 45.853 ms | 1.352x |
| 1024 | 401,934.2 | 368,496.6 | 91.68% | 18.658 ms | 24.972 ms | 1.338x |
| 65536 | 20,275.0 | 17,718.2 | 87.39% | 6.466 ms | 7.318 ms | 1.132x |
| 131072 | 12,891.4 | 11,670.8 | 90.53% | 5.797 ms | 6.187 ms | 1.067x |
| 262144 | 10,340.2 | 9,429.2 | 91.19% | 0.400 ms | 0.445 ms | 1.113x |

- throughput ratio 산술평균: **90.33%** — C++ 단순 one-way 완화 목표 90% 통과
- mean-latency ratio 산술평균: **1.195x** — C++ 2.0x 상한 통과

소스 변경 없이, 사전 결정한 5회 final paired 세트가 두 gate를 통과했으므로
**PUBSUB/ws는 완화 목표 90%로 통과**다.
