# C++ Multi `ws`·`wss` REQREP 재측정 — C 기준 runner 수정(D-B89) 뒤

[before 측정](2026-09-05-cpp-multi-tls-ws-wss-before.ko.md)에서 `보류(C 기준 이상)`로 둔 세 셀(C runner의
4096B timeout avalanche)의 원인이 C multi REQREP runner의 제출 턴(100 client 전부 제출 뒤 poller 진행)으로
확정되어 runner만 수정했다(`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp`, WS/WSS 32 KiB byte-quantum
round-robin 턴, 커밋 `bindings/c/perf: byte-quantum round-robin submit turns for WS/WSS REQREP runners`, D-B89).
C 기준값이 다섯 size 모두 바뀌므로 `ws`·`wss` REQREP 4 transport×pattern 셀을 C·C++ 모두 다시 잰다.
C++ 코드는 pass 2(`e6dd88fbc6`) 그대로다.

## 조건

- 머신: [environment](2026-09-05-environment.ko.md)와 동일, 다른 perf 작업 없음(load average C 시작 1.41, C++ 시작 0.34).
- Core 0.17.0 dev(`core/build`), C runner 수정 뒤, 100 clients, 5 s, size 64/256/1024/4096/65536.
- C: `--runs 5`(평균, 05:27~05:28, 수정 검증 체인 `b-verify-cws`의 일부), tag `p1cpp-<T>-fix`,
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_0527{07,34}_p1cpp-ws-fix.txt`,
  `…_0528{01,28}_p1cpp-wss-fix.txt`.
- C++: `--runs 1`(before와 같은 조건, 05:31~05:33), tag `p1cpp-<T>-fix`,
  `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_0531{51}_p1cpp-ws-fix.txt`, `…_053219_p1cpp-ws-fix.txt`,
  `…_053247_p1cpp-wss-fix.txt`, `…_053315_p1cpp-wss-fix.txt`.
- 비율 = C++ / C(처리량), C++ / C(평균 latency). 목표 85% / 2.0x(§7.2).

## 측정값과 비율

### `MULTI_DEALER_ROUTER_REQREP` `ws`

| size | C ops/s | C++ ops/s | ratio | C ms | C++ ms | lat ratio |
|---|---|---|---|---|---|---|
| 64 | 192,852.8 | 80,307.8 | 41.6% | 1.056 | 0.613 | 0.58x |
| 256 | 174,771.2 | 78,002.2 | 44.6% | 1.080 | 0.629 | 0.58x |
| 1024 | 144,214.8 | 73,946.8 | 51.3% | 0.888 | 0.661 | 0.74x |
| 4096 | 75,974.8 | 63,325.0 | 83.4% | 0.365 | 0.773 | 2.12x |
| 65536 | 11,010.2 | 22,191.8 | 201.6% | 0.273 | 2.219 | 8.14x |

aggregate throughput **53.1%**, latency 산술평균 **2.43x**.

### `MULTI_ROUTER_ROUTER_REQREP` `ws`

| size | C ops/s | C++ ops/s | ratio | C ms | C++ ms | lat ratio |
|---|---|---|---|---|---|---|
| 64 | 135,501.6 | 78,681.2 | 58.1% | 1.326 | 0.621 | 0.47x |
| 256 | 123,234.4 | 77,109.6 | 62.6% | 1.288 | 0.634 | 0.49x |
| 1024 | 102,385.8 | 73,887.2 | 72.2% | 1.101 | 0.660 | 0.60x |
| 4096 | 59,200.4 | 63,436.8 | 107.2% | 0.484 | 0.770 | 1.59x |
| 65536 | 10,137.2 | 18,018.8 | 177.7% | 0.285 | 2.723 | 9.55x |

aggregate throughput **72.3%**, latency 산술평균 **2.54x**.

### `MULTI_DEALER_ROUTER_REQREP` `wss`

| size | C ops/s | C++ ops/s | ratio | C ms | C++ ms | lat ratio |
|---|---|---|---|---|---|---|
| 64 | 137,407.8 | 48,730.8 | 35.5% | 2.532 | 1.012 | 0.40x |
| 256 | 122,259.0 | 47,119.0 | 38.5% | 2.343 | 1.043 | 0.45x |
| 1024 | 101,712.8 | 47,552.0 | 46.8% | 2.235 | 1.034 | 0.46x |
| 4096 | 54,901.2 | 34,864.2 | 63.5% | 1.722 | 1.413 | 0.82x |
| 65536 | 7,785.0 | 7,220.2 | 92.7% | 2.311 | 6.871 | 2.97x |

aggregate throughput **43.7%**, latency 산술평균 **1.02x**.

### `MULTI_ROUTER_ROUTER_REQREP` `wss`

| size | C ops/s | C++ ops/s | ratio | C ms | C++ ms | lat ratio |
|---|---|---|---|---|---|---|
| 64 | 115,085.0 | 41,094.8 | 35.7% | 2.994 | 1.202 | 0.40x |
| 256 | 108,599.0 | 39,850.6 | 36.7% | 3.062 | 1.238 | 0.40x |
| 1024 | 87,184.0 | 40,043.0 | 45.9% | 2.497 | 1.232 | 0.49x |
| 4096 | 47,560.0 | 35,038.4 | 73.7% | 2.052 | 1.408 | 0.69x |
| 65536 | 6,496.6 | 7,179.0 | 110.5% | 3.883 | 6.920 | 1.78x |

aggregate throughput **44.7%**, latency 산술평균 **0.75x**.

## 읽는 법·주의

- C 기준값이 바뀌었다. 64~1024B는 수정 전(ws DR 147/135/127k)보다 오히려 높고(193/175/144k; 5-run 평균, 조용한
  머신), 4096B는 붕괴가 사라진 대신 **queue 깊이가 줄어 C latency가 0.3~0.5 ms(ws)로 매우 낮다**. 그래서 ws
  4096/65536B의 latency 비율(2.1x/8.1x, RR 1.6x/9.6x)은 C++가 느려진 것이 아니라 C runner의 턴 구조가 낮은
  queue 깊이를 만든 결과다. C++ ws 65536B latency 2.2 ms는 before(4.1 ms)보다 오히려 개선됐다.
- C++ `ws` 64~1024B 처리량(80/78/74k)이 before(55/53/52k)보다 45% 높다. C++ 코드는 같고(pass 2), before는 C 직후
  load 4.6~4.8에서, 이번은 load 0.3에서 시작했다. `ws` C++ 셀은 run-to-run 편차가 크므로 after 측정 때는
  before/after를 같은 시각에 짝지어 재측정한다(§7.3).
- 판정 기준(§7.2)은 aggregate throughput 85%·latency 2.0x 둘 다이므로 `ws` 두 pattern은 latency 평균이 2.0x를
  넘어 `미달`, `wss` 두 pattern은 latency는 통과하나 throughput이 43.7/44.7%로 `미달`.

## 판정 요약 (before 대체 — 개선 pass 전, 판정 미확정)

| pattern | transport | aggregate throughput | latency 평균 | 목표 | 상태 |
|---|---|---|---|---|---|
| `MULTI_DEALER_ROUTER_REQREP` | `ws` | 53.1% | 2.43x | 85% / 2.0x | `미달(53.1%)`, 개선 pass 전 |
| `MULTI_ROUTER_ROUTER_REQREP` | `ws` | 72.3% | 2.54x | 85% / 2.0x | `미달(72.3%)`, 개선 pass 전 |
| `MULTI_DEALER_ROUTER_REQREP` | `wss` | 43.7% | 1.02x | 85% / 2.0x | `미달(43.7%)`, 개선 pass 전 |
| `MULTI_ROUTER_ROUTER_REQREP` | `wss` | 44.7% | 0.75x | 85% / 2.0x | `미달(44.7%)`, 개선 pass 전 |

`보류(C 기준 이상)` 셀 세 개는 모두 해소되어 `미달`로 바뀌었다. 계획서 §9.1 C++ Multi 표의 `ws`·`wss` REQREP
4행이 이 log 값으로 갱신됐다.
