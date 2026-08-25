# C++ Single DEALER_DEALER / tcp — Core 0.13.0

## Manifest

| 항목 | 값 |
|------|----|
| branch / source-report commit | `core-0.13.0-bindings-performance` / `efe8023e8a` |
| source version | `0.13.2` |
| Core runtime / version | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64/lib/libzlink.so.0.13.0` / `0.13.0` |
| Core release tag / revision | `core/v0.13.0` / `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB |
| CPU pin / 동시 perf process | 사용 안 함 / 없음 |

## Smoke와 paired 기준선 — 3 runs

`DEALER_DEALER / tcp / 64B`, duration 1초, runs 1에서 C와 C++ 모두 `status: complete`였다.

- smoke C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_113254_cpp-dealer-dealer-tcp-core0130-smoke-c-20260825.txt`
- smoke C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_113254_cpp-dealer-dealer-tcp-core0130-smoke-cpp-20260825.txt`
- C: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_113312_cpp-dealer-dealer-tcp-core0130-before-c-20260825.txt`
- C++: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_113312_cpp-dealer-dealer-tcp-core0130-before-cpp-20260825.txt`
- 두 paired report `status: complete`, 30/30 result lines; runtime/revision/tag, Effective Options, auto-HWM과 six sizes 일치

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 2,080,321.2 | 1,609,888.6 | 77.39% | 0.827 ms | 0.651 ms | 0.787x |
| 256 | 1,186,014.4 | 1,165,195.0 | 98.24% | 1.455 ms | 1.506 ms | 1.035x |
| 1024 | 604,777.8 | 579,976.6 | 95.90% | 0.825 ms | 0.855 ms | 1.036x |
| 65536 | 32,547.2 | 23,073.8 | 70.89% | 0.311 ms | 0.459 ms | 1.476x |
| 131072 | 20,261.6 | 16,604.2 | 81.95% | 0.306 ms | 0.383 ms | 1.252x |
| 262144 | 10,600.6 | 9,053.2 | 85.40% | 0.387 ms | 0.452 ms | 1.168x |

- throughput ratio 산술평균: **84.96%** — 기본 95%와 완화 90% 목표 모두 미달
- mean-latency ratio 산술평균: **1.126x** — C++ 2.0x 상한 통과

## 공개 경로 후보 검토

DEALER 송신은 `message_t::from`으로 payload를 소유한 뒤 pooled operation state를 통해
동기 public `send().message(...).submit()`에 도달한다. 전송 실패 시 message ownership 복원,
socket close와의 경쟁 방지, 동일 socket의 outbound attempt 직렬화는 공개 terminal의 계약이다.
`outbound_record_attempt_mutex`, callback-state weak liveness check 또는 public builder를 제거하면
64B 손실은 줄일 수 있어도 concurrent send/close와 실패 시 소유권 반환 규약을 바꾸므로 채택할 수 없다.

64KiB 이상도 `message_t::from`의 한 번의 소유 복사와 Core send handoff가 공통으로 지배한다.
재사용 payload, operation-state pool, single-part direct submit은 이미 적용돼 있으며, 큰 메시지 pool
추가/활성화는 정책상 금지다. C API/private perf path, raw native message 재사용, mutex/weak 제거,
pool 확대는 모두 no-go다.

처리량이 완화 목표보다 충분히 낮아 final 5회는 요구되지 않는다. 안전한 binding 변경 후보가 없으므로
source 변경 없이 **DEALER_DEALER/tcp는 보류**하며, transport별 활성 구간을 계속 측정한다.
