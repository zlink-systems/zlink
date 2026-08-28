# C++ Single DEALER_ROUTER / ws — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `34f4acc824` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 paired 기준선 — 3 runs

`DEALER_ROUTER / ws / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_122313_cpp-dealer-router-ws-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_122319_cpp-dealer-router-ws-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_122327_cpp-dealer-router-ws-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_122504_cpp-dealer-router-ws-core0130-before-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,463,784.6 | 1,979,616.0 | 80.35% | 40.252 ms | 45.655 ms | 1.134x |
| 256 | 1,097,945.4 | 1,065,350.0 | 97.03% | 32.004 ms | 34.231 ms | 1.070x |
| 1024 | 371,052.8 | 354,371.2 | 95.50% | 24.132 ms | 26.179 ms | 1.085x |
| 65536 | 17,895.2 | 15,296.0 | 85.48% | 7.089 ms | 7.829 ms | 1.104x |
| 131072 | 12,317.0 | 11,287.8 | 91.64% | 0.485 ms | 0.538 ms | 1.109x |
| 262144 | 7,432.2 | 7,097.6 | 95.50% | 0.551 ms | 0.585 ms | 1.062x |

- throughput ratio 산술평균: **90.92%** — routed one-way 목표 85% 통과
- mean-latency ratio 산술평균: **1.094x** — C++ 2.0x 상한 통과
- 모든 size가 routed one-way 개별 최소 80%를 통과했다. 목표 경계가 아니므로 최종 5회는 요구되지 않는다.

## 공개 경로 검토

WebSocket handshake와 connection-ready 이전의 setup은 활성 측정 구간 밖이며, 활성 구간은
TCP와 같은 public `send_routed` 및 Router `recv(routing_id_t&, message_t&, dontwait)` 경로다.
routing id와 payload ownership, poller readiness 소비, transient failure와 wire stop token은
공개 terminal 계약이다. C API/private path, native routing-id/message 재사용, ownership/close
semantic 변경, weak/mutex 제거, 큰 메시지 전용 pool 증설은 모두 no-go다.

제거 가능한 binding 비용 후보가 없고 기준선이 목표와 latency 상한을 모두 충족했으므로 source
변경 없이 **DEALER_ROUTER/ws는 통과**로 확정한다.
