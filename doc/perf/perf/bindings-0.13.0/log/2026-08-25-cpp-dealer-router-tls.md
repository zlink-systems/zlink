# C++ Single DEALER_ROUTER / tls — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `62c44b9cfd` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 최종 paired 판정 — secure transport 5 runs

`DEALER_ROUTER / tls / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_123509_cpp-dealer-router-tls-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_123516_cpp-dealer-router-tls-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_123528_cpp-dealer-router-tls-core0130-final5-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_123806_cpp-dealer-router-tls-core0130-final5-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

secure transport 의무 규칙에 따라 C→C++ 순서의 5회 중앙값을 최종 판정에 사용했다.

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,390,154.6 | 2,186,430.6 | 91.48% | 1.855 ms | 0.473 ms | 0.255x |
| 256 | 862,228.8 | 842,442.0 | 97.71% | 1.982 ms | 2.006 ms | 1.012x |
| 1024 | 265,488.0 | 262,126.6 | 98.73% | 1.859 ms | 1.875 ms | 1.009x |
| 65536 | 10,720.0 | 8,813.4 | 82.21% | 0.915 ms | 1.144 ms | 1.250x |
| 131072 | 6,271.2 | 5,436.2 | 86.69% | 0.928 ms | 1.109 ms | 1.195x |
| 262144 | 3,490.0 | 2,811.4 | 80.56% | 1.135 ms | 1.399 ms | 1.232x |

- throughput ratio 산술평균: **89.56%** — routed one-way 목표 85% 통과
- mean-latency ratio 산술평균: **0.992x** — C++ 2.0x 상한 통과
- 모든 size가 routed one-way 개별 최소 80%를 통과했다.

## 공개 경로 검토

TLS setup/connection-ready는 활성 구간 밖이고, 활성 구간은 public `send_routed`와 Router
`recv(routing_id_t&, message_t&, dontwait)`다. routing id/payload ownership, poller readiness,
transient failure, wire stop token 및 close/liveness/serialization은 공개 terminal 계약이다.
C API/private path, native message/routing-id 재사용, ownership/close semantic 변경, weak/mutex
제거, 큰 메시지 전용 pool 증설은 no-go다.

제거 가능한 binding 비용 후보가 없고 secure transport 최종 5회 결과가 목표와 latency 상한을
모두 충족했으므로 source 변경 없이 **DEALER_ROUTER/tls는 통과**로 확정한다.
