# C++ Single DEALER_ROUTER / wss — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `ea00c7203c` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최종 paired 판정 — secure transport 5 runs

`DEALER_ROUTER / wss / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_122802_cpp-dealer-router-wss-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_122808_cpp-dealer-router-wss-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_122817_cpp-dealer-router-wss-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_123055_cpp-dealer-router-wss-core0130-final5-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

secure transport 의무 규칙에 따라 초기 3회 기준선 없이 C→C++ 순서의 5회 중앙값을 최종
판정에 사용했다.

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,985,167.4 | 1,911,366.0 | 96.28% | 50.092 ms | 54.914 ms | 1.096x |
| 256 | 692,816.2 | 681,658.2 | 98.39% | 40.180 ms | 43.080 ms | 1.072x |
| 1024 | 208,517.8 | 203,697.2 | 97.69% | 32.846 ms | 35.301 ms | 1.075x |
| 65536 | 7,809.6 | 7,594.8 | 97.25% | 16.646 ms | 17.932 ms | 1.077x |
| 131072 | 4,665.0 | 4,148.2 | 88.92% | 13.678 ms | 14.732 ms | 1.077x |
| 262144 | 2,484.8 | 2,340.0 | 94.17% | 13.046 ms | 14.334 ms | 1.099x |

- throughput ratio 산술평균: **95.45%** — routed one-way 목표 85% 통과
- mean-latency ratio 산술평균: **1.083x** — C++ 2.0x 상한 통과
- 모든 size가 routed one-way 개별 최소 80%를 통과했다.

## 공개 경로 검토

WSS handshake/TLS 설정 및 connection-ready 대기는 활성 구간 밖이고, 활성 구간은 public
`send_routed`와 Router `recv(routing_id_t&, message_t&, dontwait)`다. routing-id/message
ownership, poller readiness, transient failure, wire stop token, close/liveness와 outbound
serialization은 공개 terminal 계약이므로 제거·대체할 수 없다.

C API/private path, native message 또는 routing-id 재사용, ownership/close semantic 변경,
weak/mutex 제거, 큰 메시지 전용 pool 증설은 no-go다. 최종 5회 결과가 목표와 latency 상한을
통과했으므로 source 변경 없이 **DEALER_ROUTER/wss는 통과**로 확정한다.
