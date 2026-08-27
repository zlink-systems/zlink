# C++ Single PAIR / tcp — local Core 0.13.2

## Manifest과 준비

- Core: local `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`, revision
  `7361d8060727dc090f6224a9440872d8b56d5942`, `core_dirty=1`.
- Core build: `cmake --build core/build -j 4` → `ninja: no work to do`.
- C reference와 C++ binding runner build, 64B smoke를 C→C++ 순으로 완료했다.
  - C: `1135904 msg/s`, `0.199 ms`.
  - C++: `887863 msg/s`, `59.895 ms`.
- Paired manifest: C→C++, `--pattern PAIR --transports tcp --msg-sizes
  64,256,1024,65536,131072,262144 --duration 5 --runs 5`; 1 client, balanced auto-HWM,
  automatic HWM, one I/O thread, two application parts.

## Before

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260827_003852_cpp-pair-tcp-local0132-run-20260827-before-c.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260827_004159_cpp-pair-tcp-local0132-run-20260827-before-cpp.txt`
- Throughput ratios: `74.63%, 81.40%, 88.20%, 94.02%, 97.57%, 97.61%`; aggregate
  **88.90%**.
- Mean-latency ratio median: **11.47x**. Throughput aggregate와 latency 상한(2.0x) 모두
  미달했다.

## PAIR runner 대조와 후보 A

C runner는 poller가 준비됐을 때 raw part를 `DONTWAIT`으로 받고, 첫 payload 뒤의 빈
두 번째 part를 즉시 받아 검증한다. C++ runner도 같은 poller/start/stop/drain 경계,
two-part wire shape, send retry 및 timeout을 사용한다.

차이는 C++ runner의 수신 inner loop가 매 메시지마다 새 `received_t`를 만들었다는 점이다.
그 결과 two-part `received_t::parts()`의 vector capacity가 매번 파괴되어, C reference에는
없는 envelope 재할당과 초기화가 발생했다. `socket_t::receive()`는 caller-provided
`received_t`의 storage를 재사용하도록 구현되어 있으므로, PAIR runner에서 receiver thread
수명 동안 하나의 `received_t`를 재사용하도록 바꿨다. 측정 대상, public API, payload,
warmup/측정 경계, 동기화 및 send/receive 조건은 바꾸지 않았다.

POSDDD 관점에서 이 변경은 receive envelope storage의 소유자와 수명을 receiver loop에
일치시켜 불필요한 상태 생성과 heap allocation을 제거한다. 새 abstraction이나 public
interface는 만들지 않았고, multipart 순서·stop token·failure 처리도 그대로 유지한다.

## Candidate A 재측정

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260827_004556_cpp-pair-tcp-local0132-run-20260827-candidate-a-c.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260827_004838_cpp-pair-tcp-local0132-run-20260827-candidate-a-cpp.txt`

| Size | C msg/s | C++ msg/s | Throughput ratio | C latency ms | C++ latency ms | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 980988.6 | 779687.2 | 79.48% | 19.402 | 93.822 | 4.84x |
| 256 | 897365.2 | 763161.8 | 85.04% | 0.511 | 34.678 | 67.86x |
| 1024 | 804015.6 | 755139.8 | 93.92% | 0.666 | 6.133 | 9.21x |
| 65536 | 37146.0 | 34197.6 | 92.06% | 0.243 | 0.289 | 1.19x |
| 131072 | 26277.4 | 25851.6 | 98.38% | 0.213 | 0.216 | 1.01x |
| 262144 | 15972.0 | 15831.4 | 99.12% | 0.215 | 0.218 | 1.01x |

- Throughput aggregate mean: **91.33%** (before 대비 +2.43%p, 목표 95.00% 미달).
- Mean-latency ratio median: **3.01x** (상한 2.0x 미달).
- Size-level result line은 각 5-run median이며, report가 원시 반복값을 보존한다.
  - C 64B throughput: `1076.53, 973.94, 944.83, 980.99, 989.69 Kmsg/s`; C++:
    `812.61, 771.85, 779.69, 767.35, 783.24 Kmsg/s`.
  - C 256B throughput: `999.68, 894.27, 907.07, 897.37, 876.81 Kmsg/s`; C++:
    `665.04, 757.03, 772.89, 779.04, 763.16 Kmsg/s`.
  - C 1024B throughput: `882.48, 793.79, 844.39, 804.02, 622.01 Kmsg/s`; C++:
    `755.14, 644.06, 755.87, 755.25, 728.63 Kmsg/s`.
  - C 65536B throughput: `38.95, 37.97, 37.15, 37.12, 31.76 Kmsg/s`; C++:
    `34.54, 34.20, 33.44, 34.54, 33.52 Kmsg/s`.
  - C 131072B throughput: `26.91, 26.48, 26.28, 25.26, 19.66 Kmsg/s`; C++:
    `25.85, 25.35, 26.00, 25.89, 25.35 Kmsg/s`.
  - C 262144B throughput: `16.41, 14.39, 16.05, 15.97, 14.04 Kmsg/s`; C++:
    `15.88, 15.68, 15.83, 16.07, 15.81 Kmsg/s`.

## 남은 후보와 판정

- `socket_callback_state_t::outbound_record_attempt_mutex`는 `socket_t::send`, routed send,
  publish, received send/reply와 close가 공유하는 모든 송신 경로의 lifetime/close gate다.
  이 row 전용 변경이 아니므로 cross-cutting 후보로만 기록하며 구현하지 않았다.
- `operation_state_t` acquire/recycle와 `socket_t::receive_impl`의 reusable envelope도 PAIR만이
  아닌 공통 send/receive 경로다. API/ownership/close/callback context에 영향을 주므로 이 row에서
  바꾸지 않았다.
- PAIR 전용의 안전한 allocation/copy/synchronization 후보는 위 runner storage 재사용 후에는
  찾지 못했다. 따라서 상태는 감독관 판정 전까지 **미달(91.33%)**이며 다음 pattern으로 진행하지 않는다.

## Contract 검증

`ctest --test-dir bindings/cpp/build -L contract --output-on-failure`: **14/14 통과**, 5.30초.
