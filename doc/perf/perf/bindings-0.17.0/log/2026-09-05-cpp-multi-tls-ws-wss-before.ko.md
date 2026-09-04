# C++ Multi `tls`·`ws`·`wss` 4 pattern before 측정 — Core 0.17.0

2026-09-05 04:41~04:55 KST에 C++ Multi suite의 나머지 세 transport(`tls`, `ws`, `wss`)에 대해
첫 paired 측정을 실행했다. 이 기록은 세 transport에 대한 개선 pass 전의 **before** 측정이다.
C++ 코드는 `tcp` pass 2(`log/2026-09-05-cpp-multi-tcp-pass2.ko.md`, 커밋 `e6dd88fbc6`)를 포함한
상태이며, 이 세 transport를 대상으로 한 자체 pass 1과 Sol 리뷰 pass 2는 아직 수행하지 않았다.
따라서 계획서 §9.1.2의 셀은 임시 상태로만 갱신했고 transport 판정은 닫지 않았다.

이 세션에서 C 기준 runner 쪽의 4096B 이상 현상을 세 셀에서 확인했다(아래 "C 기준 이상"). 해당
셀은 C++ 결과가 아니라 C 기준(reference runner 또는 Core) 쪽 blocker로 기록하고 비교에서 제외한다.

## Manifest

| 항목 | 값 |
|------|----|
| source | `main` / `6e8d798bac`(`tcp` pass 2 커밋 `e6dd88fbc6` 포함). `wss` 8개 report는 `296c5c04e5`(계획서·로그 문서 2개만 변경, `core/`·`bindings/` diff 0줄) |
| Core runtime | `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` (`tcp` before·pass 1·pass 2와 동일 artifact) |
| Core build / version / revision | local `Release` + LTO / `0.17.0` / `META,core_revision` `6e8d798bac05248471bd589d571758eb4ccceed5`(`tls`·`ws`), `296c5c04e55b6ed1da572eda9d4dc1cf8d326568`(`wss`), 모두 `core_dirty=0` |
| binding | `zlink_cpp` 0.17.0, GCC/G++ 13.3.0 (`bindings/cpp/build` incremental) |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB (`log/2026-09-05-environment.ko.md`), `tcp` 측정과 같은 boot session |
| CPU 상태 | governor 미노출, `--pin-cpu` 없음, 동시 perf process 없음 |
| pair tag | `p1cpp-tls`, `p1cpp-ws`, `p1cpp-wss` |

- 24개 report의 `META,core_runtime`은 모두 같은 경로였고 `core_dirty=0`이었다. `core_revision`은 `ws`
  `MULTI_PUBSUB` C++(04:50:29)까지 `6e8d798bac`, `wss` `MULTI_DEALER_DEALER` C(04:51:04)부터 `296c5c04e5`다.
  두 commit 사이의 diff는 `doc/perf/perf/bindings-0.17.0/` 문서 2개뿐이므로 같은 Core artifact·같은 binding
  코드다. 같은 pattern의 C와 C++ pair는 항상 같은 revision이다.
- report는 binding 작업 tree의 dirty 여부를 기록하지 않는다. 측정 시점의 C++ source는 `META,commit`이 가리키는
  commit의 pass 2 코드다.

## 명령

transport마다 `DEALER_DEALER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`, `PUBSUB` 순서로, pattern마다 C를
먼저 실행하고 종료 직후 같은 조건으로 C++를 실행했다. `tls` → `ws` → `wss` 순서로 24회를 순차 실행했다.

```bash
# <T> = tls | ws | wss, <PATTERN> = DEALER_DEALER | DEALER_ROUTER_REQREP | ROUTER_ROUTER_REQREP | PUBSUB
ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=4 PERF_MULTI_TIMEOUT_SECONDS=900 \
  bash bindings/c/perf/run_benchmarks_multi.sh --pattern <PATTERN> --transports <T> \
  --msg-sizes 64,256,1024,4096,65536 --clients 100 --duration 5 --runs 1 --results-tag p1cpp-<T>

ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$HOME/project/zlink/core/build \
  ZLINK_BUILD_JOBS=4 PERF_MULTI_TIMEOUT_SECONDS=900 \
  bash bindings/cpp/perf/run_benchmarks_multi.sh --pattern <PATTERN> --transports <T> \
  --msg-sizes 64,256,1024,4096,65536 --duration 5 --runs 1 --results-tag p1cpp-<T>
```

- C++ 명령은 `--clients`를 생략했지만 Effective Options의 `clients: 100`, `default_clients: 100`은 C와 같다.
- `tcp` before와 같이 §7.1의 별도 smoke 실행 기록은 없다. 본 측정 report 24개가 모두 `status: complete`였다.
- §3.2의 6개 크기 중 `131072`는 이번에도 제외했다. `131072` 셀은 `미측정`으로 남긴다.

## 조건

| 항목 | 값 |
|------|----|
| suite / transport | Multi / `tls`, `ws`, `wss` |
| pattern | `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`, `MULTI_PUBSUB` |
| size | 64, 256, 1024, 4096, 65536 |
| duration / runs | 5초 / 1회(`runs=1`이므로 측정값을 그대로 사용) |
| clients | 100 (`service_clients: auto`) |
| I/O threads | server 4, client 4 (default) |
| HWM | `auto-hwm`, `ctx_auto_hwm_profile: balanced`, `sndbuf/rcvbuf: -1` |
| timeout | `PERF_MULTI_TIMEOUT_SECONDS=900` |

## Report

모든 report는 `status: complete`, `success: 5`, `expected_result_lines: 25`, `actual_result_lines: 25`였다.
C report는 `bindings/c/perf/results/multi/report/`, C++ report는 `bindings/cpp/perf/results/multi/report/`에 있다.

| Transport | Pattern | C report | C++ report |
|-----------|---------|----------|------------|
| `tls` | `MULTI_DEALER_DEALER` | `perf_c_multi_linux_20260905_044211_p1cpp-tls.txt` | `perf_cpp_multi_linux_20260905_044349_p1cpp-tls.txt` |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_044417_p1cpp-tls.txt` | `perf_cpp_multi_linux_20260905_044446_p1cpp-tls.txt` |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_044513_p1cpp-tls.txt` | `perf_cpp_multi_linux_20260905_044542_p1cpp-tls.txt` |
| `tls` | `MULTI_PUBSUB` | `perf_c_multi_linux_20260905_044610_p1cpp-tls.txt` | `perf_cpp_multi_linux_20260905_044638_p1cpp-tls.txt` |
| `ws` | `MULTI_DEALER_DEALER` | `perf_c_multi_linux_20260905_044714_p1cpp-ws.txt` | `perf_cpp_multi_linux_20260905_044743_p1cpp-ws.txt` |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_044810_p1cpp-ws.txt` | `perf_cpp_multi_linux_20260905_044839_p1cpp-ws.txt` |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_044906_p1cpp-ws.txt` | `perf_cpp_multi_linux_20260905_044934_p1cpp-ws.txt` |
| `ws` | `MULTI_PUBSUB` | `perf_c_multi_linux_20260905_045001_p1cpp-ws.txt` | `perf_cpp_multi_linux_20260905_045029_p1cpp-ws.txt` |
| `wss` | `MULTI_DEALER_DEALER` | `perf_c_multi_linux_20260905_045104_p1cpp-wss.txt` | `perf_cpp_multi_linux_20260905_045133_p1cpp-wss.txt` |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_045202_p1cpp-wss.txt` | `perf_cpp_multi_linux_20260905_045231_p1cpp-wss.txt` |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_045258_p1cpp-wss.txt` | `perf_cpp_multi_linux_20260905_045327_p1cpp-wss.txt` |
| `wss` | `MULTI_PUBSUB` | `perf_c_multi_linux_20260905_045355_p1cpp-wss.txt` | `perf_cpp_multi_linux_20260905_045424_p1cpp-wss.txt` |

실행 시각(`META,timestamp`, KST)과 시작 시점 load average(`META,load_avg`, 1/5/15분):

| 순서 | 실행 | 시각 | load_avg |
|------|------|------|----------|
| 1 | C `DEALER_DEALER` `tls` | 04:42:11 | 0.73 1.21 1.19 |
| 2 | C++ `DEALER_DEALER` `tls` | 04:43:49 | 1.76 1.55 1.32 |
| 3 | C `DEALER_ROUTER_REQREP` `tls` | 04:44:17 | 2.40 1.71 1.38 |
| 4 | C++ `DEALER_ROUTER_REQREP` `tls` | 04:44:46 | 3.69 2.10 1.52 |
| 5 | C `ROUTER_ROUTER_REQREP` `tls` | 04:45:13 | 6.16 2.80 1.77 |
| 6 | C++ `ROUTER_ROUTER_REQREP` `tls` | 04:45:42 | 6.16 3.12 1.91 |
| 7 | C `PUBSUB` `tls` | 04:46:10 | 8.50 3.94 2.21 |
| 8 | C++ `PUBSUB` `tls` | 04:46:38 | 6.91 4.01 2.29 |
| 9 | C `DEALER_DEALER` `ws` | 04:47:14 | 6.29 4.16 2.40 |
| 10 | C++ `DEALER_DEALER` `ws` | 04:47:43 | 4.96 4.02 2.41 |
| 11 | C `DEALER_ROUTER_REQREP` `ws` | 04:48:10 | 4.61 4.00 2.46 |
| 12 | C++ `DEALER_ROUTER_REQREP` `ws` | 04:48:39 | 4.77 4.08 2.53 |
| 13 | C `ROUTER_ROUTER_REQREP` `ws` | 04:49:06 | 5.47 4.34 2.66 |
| 14 | C++ `ROUTER_ROUTER_REQREP` `ws` | 04:49:34 | 5.71 4.49 2.76 |
| 15 | C `PUBSUB` `ws` | 04:50:01 | 7.55 5.09 3.02 |
| 16 | C++ `PUBSUB` `ws` | 04:50:29 | 6.57 5.05 3.06 |
| 17 | C `DEALER_DEALER` `wss` | 04:51:04 | 5.47 4.94 3.09 |
| 18 | C++ `DEALER_DEALER` `wss` | 04:51:33 | 4.21 4.68 3.06 |
| 19 | C `DEALER_ROUTER_REQREP` `wss` | 04:52:02 | 4.14 4.60 3.09 |
| 20 | C++ `DEALER_ROUTER_REQREP` `wss` | 04:52:31 | 4.48 4.64 3.15 |
| 21 | C `ROUTER_ROUTER_REQREP` `wss` | 04:52:58 | 6.26 5.06 3.33 |
| 22 | C++ `ROUTER_ROUTER_REQREP` `wss` | 04:53:27 | 5.96 5.10 3.40 |
| 23 | C `PUBSUB` `wss` | 04:53:55 | 7.38 5.53 3.59 |
| 24 | C++ `PUBSUB` `wss` | 04:54:24 | 7.02 5.61 3.68 |

1분 load average 0.7~8.5는 다른 작업이 아니라 순차 실행한 perf 실행 자체가 만든 값이다(`tcp` before와
같은 양상). 같은 pattern·transport의 C와 C++는 27~98초 간격으로 연속 실행했다(첫 C++ `tls` 실행만 pass 2
코드의 incremental 재빌드로 98초). 뒤로 갈수록 5분·15분 load average가 높아진 것은 24회 연속 실행의 누적이며,
같은 pair 안에서 C와 C++가 보는 host 상태는 가깝다.

## Effective Options와 auto-HWM 대조

- 일치: `runs`, `patterns`, `transports`, `msg_sizes`, `duration_seconds`, `clients`, `default_clients`,
  `service_clients`, `server_io_threads`, `client_io_threads`, `hwm/sndhwm/rcvhwm`, `sndbuf/rcvbuf`,
  `ctx_auto_hwm_enable`, `ctx_auto_hwm_profile`, `stream_non_tcp_clients_max`, `timeout_seconds`.
- 표기만 다른 항목(`tcp`와 동일): C `routed_echo_per_socket_payload: none` / C++ `routed_echo_borrow_payload: none`,
  C `monitor_hwm_bytes: 4096000` / C++ `monitor_hwm: 4096000`.
- 값이 다른 항목(`tcp`와 동일): `default_stream_clients` C 100 / C++ 10000. `MULTI_STREAM`에만 쓰이는 값이라
  이번 4 pattern의 판정에는 영향이 없다.
- auto-HWM detail의 `MsgUnit(B)`는 C와 C++ 모두 모든 행이 `?`다. 값으로 일치 여부를 확인할 수 없어 진단 항목으로
  남긴다. C++ `MULTI_DEALER_ROUTER_REQREP`와 `MULTI_ROUTER_ROUTER_REQREP` report에는 여섯 개 모두 `Auto-HWM detail`
  블록이 없다(C REQREP report에는 있다). `tcp`와 같은 상태다.
- effective SNDHWM/RCVHWM: client 측은 모두 1048576(PUBSUB sub는 2097152)으로 같았다. server 측은 C `DEALER_DEALER`·
  `DEALER_ROUTER_REQREP`·`PUBSUB`(`tls`·`ws`) 1048576, C `ROUTER_ROUTER_REQREP`·`PUBSUB`(`wss`) 4096000, C++
  server는 대부분 4096000(`ws` `DEALER_DEALER` 256B만 1048576)이었다. 실행 시점 memory budget으로 계산된 값이며
  판정 gate는 아니지만 진단 자료로 기록한다.
- memory guard cap 발생 기록은 어느 report에도 없다. 실제 client 수는 24개 report 모두 100이다.

## C 기준 이상 (reference runner / Core 쪽 blocker)

C runner의 `4096B` REQREP 셀 세 개가 인접 크기와 단절된 값으로 무너졌다. 같은 조건의 C++는 정상이다.

| Transport | Pattern | C 1024B | C 4096B | C 65536B | C++ 4096B |
|-----------|---------|--------:|--------:|---------:|----------:|
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 127,386.8 ops/s / 1.978 ms | **7,867.2 ops/s / 58.623 ms** | 16,182.8 ops/s / 4.102 ms | 51,107.0 ops/s / 0.954 ms |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 102,773.2 ops/s / 3.954 ms | **3,413.4 ops/s / 22.360 ms** | 6,805.2 ops/s / 12.542 ms | 32,961.4 ops/s / 1.499 ms |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 102,266.6 ops/s / 4.956 ms | **16,836.6 ops/s / 20.395 ms** | 7,240.6 ops/s / 11.838 ms | 35,799.0 ops/s / 1.379 ms |

- 세 셀 모두 C 4096B 처리량이 1024B의 3~16%로 떨어지고 평균 latency가 5~30배 커진다. `ws`·`wss`
  `DEALER_ROUTER_REQREP`은 4096B가 65536B보다도 낮다. C++는 같은 4096B에서 1024B의 88~98%를 유지한다.
- 같은 C runner의 `tcp`(128,155.6 / 103,833.8 ops/s)와 `tls`(87,525.6 / 81,479.0 ops/s), `ws`
  `ROUTER_ROUTER_REQREP`(104,108.6 ops/s)의 4096B는 정상 범위다. `tls` `DEALER_ROUTER_REQREP` 4096B는 처리량은
  유지하지만 평균 latency가 1024B 1.964 ms → 33.691 ms로 뛰었고 `tls` `ROUTER_ROUTER_REQREP`도 2.770 → 8.383 ms다.
  이 두 셀은 처리량이 무너지지 않아 비교 셀로 유지하되, Core/runner 조사의 참고 신호로 함께 남긴다.
- 원인은 이 문서에서 확정하지 않는다. binding 측정이 아닌 C reference runner 또는 Core의 WebSocket 계열 4096B
  경계 문제이며, §5의 "Core 버그이면 이 작업에서 source를 다시 build해 측정 runtime을 바꾸지 않는다"에 따라
  이 작업에서는 Core를 수정하거나 재빌드하지 않는다. 별도 Core/runner 조사 job을 열었다(brief `core-c-ws-reqrep-4k`).
- §7.1·§7.3에 따라 C 기준값이 이상인 셀은 비교 근거로 쓸 수 없다. 해당 C++ 셀은 `보류(C 기준 이상)`로 표시하고
  수치만 기록하며, 세 transport·pattern의 5 size aggregate는 확정하지 않는다. 4096B를 제외한 4 size 평균은
  참고값으로만 적는다. 조사 결과에 따라 Core 또는 runner가 바뀌면 §6·§7.3에 따라 해당 pattern의 C와 C++를 같은
  manifest로 다시 paired 측정한다.

## 측정값과 비율

비율은 `C++ / C`다. 판정 입력은 throughput ratio 산술평균과 평균 latency ratio 산술평균이며, 중앙값은 보조
자료다. p95/p99는 report에 있고 진단용으로만 둔다. 목표(§2.1/§2.2, C++): 단순 one-way `최소 85% / 중앙값 95%`,
socket request/reply `최소 75% / 중앙값 85%`, 평균 latency 2.0x 이하. §2.1의 완화 목표 90%는 개선 pass 전이므로
어느 transport에도 선택하지 않았다.

### `MULTI_DEALER_DEALER` (단순 one-way)

`tls`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,056,135.0 | 680,057.0 | 64.4% | 55.761 ms | 1.732 ms | 0.03x |
| 256 | 932,139.8 | 668,437.0 | 71.7% | 2.450 ms | 0.809 ms | 0.33x |
| 1024 | 688,633.6 | 580,875.4 | 84.4% | 27.708 ms | 719.255 ms | 25.96x |
| 4096 | 345,940.0 | 229,581.8 | 66.4% | 53.918 ms | 214.268 ms | 3.97x |
| 65536 | 26,384.0 | 26,730.2 | 101.3% | 92.778 ms | 81.345 ms | 0.88x |

- throughput ratio 산술평균 **77.6%**(중앙값 71.7%) — 목표 95% 미달. 개별 최소 85% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **6.23x**(중앙값 0.88x) — 2.0x 상한 **초과**. 1024B 25.96x(C 27.7 ms, C++ 719.3 ms)와
  4096B 3.97x가 상한을 넘는 latency outlier이며 이 두 셀이 산술평균을 끌어올렸다. DD의 평균 latency는 양쪽 모두
  queue 깊이에 따라 ms 단위와 수백 ms 단위를 오가므로(`tcp` 4096B에서 C 649 ms / C++ 822 ms) 절대값보다 pair 안의
  queue 상태 차이로 본다. 개선 pass의 after에서 latency aggregate를 다시 판정한다.

`ws`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 839,241.8 | 616,652.8 | 73.5% | 90.457 ms | 1.843 ms | 0.02x |
| 256 | 884,366.6 | 592,871.2 | 67.0% | 1.928 ms | 0.575 ms | 0.30x |
| 1024 | 789,153.8 | 580,995.2 | 73.6% | 1.735 ms | 1.858 ms | 1.07x |
| 4096 | 284,343.2 | 276,216.8 | 97.1% | 851.362 ms | 905.322 ms | 1.06x |
| 65536 | 50,398.8 | 57,010.4 | 113.1% | 43.751 ms | 31.204 ms | 0.71x |

- throughput ratio 산술평균 **84.9%**(중앙값 73.6%) — 목표 95% 미달. 개별 최소 85% 미달: 64, 256, 1024.
- 평균 latency ratio 산술평균 **0.63x**(중앙값 0.71x) — 2.0x 상한 이내.

`wss`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 936,929.2 | 665,086.6 | 71.0% | 85.487 ms | 58.167 ms | 0.68x |
| 256 | 821,594.2 | 601,226.6 | 73.2% | 2.764 ms | 0.574 ms | 0.21x |
| 1024 | 441,403.6 | 542,873.8 | 123.0% | 116.903 ms | 655.170 ms | 5.60x |
| 4096 | 220,907.6 | 215,671.2 | 97.6% | 98.590 ms | 215.943 ms | 2.19x |
| 65536 | 20,491.0 | 21,509.0 | 105.0% | 131.606 ms | 110.847 ms | 0.84x |

- throughput ratio 산술평균 **93.9%**(중앙값 97.6%) — 기본 목표 95% 미달(1.1%p). 개별 최소 85% 미달: 64, 256.
  완화 목표 90%는 개선 pass 전이라 선택하지 않는다(`tcp` `MULTI_PUBSUB`과 같은 이유).
- 평균 latency ratio 산술평균 **1.91x**(중앙값 0.84x) — 2.0x 상한 이내이지만 여유가 0.09x다. 1024B 5.60x, 4096B 2.19x는
  개별 상한을 넘는 latency outlier로 기록한다.
- 1024B C 441,403.6 msg/s는 같은 pattern의 `tls` 688,633.6·`ws` 789,153.8보다 낮다. 이 셀의 123.0%는 C 쪽이 낮아서
  나온 비율이므로 C++ 개선 근거로 쓰지 않는다.

### `MULTI_DEALER_ROUTER_REQREP` (socket request/reply)

`tls`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 123,946.4 | 56,216.6 | 45.4% | 1.805 ms | 0.870 ms | 0.48x |
| 256 | 125,281.8 | 52,877.0 | 42.2% | 1.867 ms | 0.926 ms | 0.50x |
| 1024 | 112,354.8 | 47,522.6 | 42.3% | 1.964 ms | 1.034 ms | 0.53x |
| 4096 | 87,525.6 | 40,284.4 | 46.0% | 33.691 ms | 1.222 ms | 0.04x |
| 65536 | 8,836.4 | 8,518.0 | 96.4% | 9.421 ms | 5.821 ms | 0.62x |

- throughput ratio 산술평균 **54.5%**(중앙값 45.4%) — 목표 85% 미달. 개별 최소 75% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.43x**(중앙값 0.50x) — 2.0x 상한 이내. 4096B의 0.04x는 C latency 33.7 ms(위 "C 기준
  이상" 참고 신호)에서 나온 값이며 C 처리량이 유지되어 비교 셀로 둔다.

`ws` — 4096B는 C 기준 이상으로 비교 제외

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 147,207.6 | 54,959.8 | 37.3% | 1.462 ms | 0.888 ms | 0.61x |
| 256 | 134,724.0 | 53,052.6 | 39.4% | 1.520 ms | 0.921 ms | 0.61x |
| 1024 | 127,386.8 | 51,909.0 | 40.7% | 1.978 ms | 0.945 ms | 0.48x |
| 4096 | 7,867.2 (C 기준 이상) | 51,107.0 | (649.6%, 비교 제외) | 58.623 ms | 0.954 ms | (0.02x, 비교 제외) |
| 65536 | 16,182.8 | 16,649.2 | 102.9% | 4.102 ms | 2.963 ms | 0.72x |

- 5 size aggregate는 확정하지 않는다. 4096B를 제외한 4 size 참고 평균: throughput **55.1%**(중앙값 40.1%), latency **0.60x**.
  참고 평균도 목표 85% 미달이며 개별 최소 75% 미달: 64, 256, 1024.
- 4096B C++ 51,107.0 ops/s는 1024B의 98.5%로 정상이다.

`wss` — 4096B는 C 기준 이상으로 비교 제외

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 118,213.2 | 47,046.4 | 39.8% | 2.538 ms | 1.043 ms | 0.41x |
| 256 | 114,228.4 | 42,137.2 | 36.9% | 2.694 ms | 1.169 ms | 0.43x |
| 1024 | 102,773.2 | 41,344.6 | 40.2% | 3.954 ms | 1.193 ms | 0.30x |
| 4096 | 3,413.4 (C 기준 이상) | 32,961.4 | (965.6%, 비교 제외) | 22.360 ms | 1.499 ms | (0.07x, 비교 제외) |
| 65536 | 6,805.2 | 7,715.0 | 113.4% | 12.542 ms | 6.427 ms | 0.51x |

- 5 size aggregate는 확정하지 않는다. 4 size 참고 평균: throughput **57.6%**(중앙값 40.0%), latency **0.41x**.
  참고 평균도 목표 85% 미달이며 개별 최소 75% 미달: 64, 256, 1024.

### `MULTI_ROUTER_ROUTER_REQREP` (socket request/reply)

`tls`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 102,252.8 | 55,320.0 | 54.1% | 2.294 ms | 0.889 ms | 0.39x |
| 256 | 102,612.4 | 51,286.2 | 50.0% | 2.305 ms | 0.960 ms | 0.42x |
| 1024 | 102,416.8 | 47,652.0 | 46.5% | 2.770 ms | 1.035 ms | 0.37x |
| 4096 | 81,479.0 | 41,626.4 | 51.1% | 8.383 ms | 1.185 ms | 0.14x |
| 65536 | 9,160.8 | 9,217.0 | 100.6% | 8.911 ms | 5.388 ms | 0.60x |

- throughput ratio 산술평균 **60.5%**(중앙값 51.1%) — 목표 85% 미달. 개별 최소 75% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.38x**(중앙값 0.39x) — 2.0x 상한 이내.

`ws`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 130,668.4 | 61,709.8 | 47.2% | 1.346 ms | 0.793 ms | 0.59x |
| 256 | 127,183.0 | 57,872.2 | 45.5% | 1.341 ms | 0.846 ms | 0.63x |
| 1024 | 114,923.4 | 57,064.2 | 49.7% | 1.546 ms | 0.855 ms | 0.55x |
| 4096 | 104,108.6 | 49,704.0 | 47.7% | 2.582 ms | 0.983 ms | 0.38x |
| 65536 | 17,660.0 | 17,647.4 | 99.9% | 3.454 ms | 2.772 ms | 0.80x |

- throughput ratio 산술평균 **58.0%**(중앙값 47.7%) — 목표 85% 미달. 개별 최소 75% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.59x**(중앙값 0.59x) — 2.0x 상한 이내.
- 같은 `ws`에서 C `ROUTER_ROUTER_REQREP` 4096B는 정상(104,108.6 ops/s)이고 C `DEALER_ROUTER_REQREP` 4096B만 무너졌다.

`wss` — 4096B는 C 기준 이상으로 비교 제외

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 116,226.6 | 49,461.8 | 42.6% | 3.059 ms | 0.996 ms | 0.33x |
| 256 | 107,362.6 | 42,557.6 | 39.6% | 2.772 ms | 1.160 ms | 0.42x |
| 1024 | 102,266.6 | 40,582.4 | 39.7% | 4.956 ms | 1.215 ms | 0.25x |
| 4096 | 16,836.6 (C 기준 이상) | 35,799.0 | (212.6%, 비교 제외) | 20.395 ms | 1.379 ms | (0.07x, 비교 제외) |
| 65536 | 7,240.6 | 7,224.4 | 99.8% | 11.838 ms | 6.878 ms | 0.58x |

- 5 size aggregate는 확정하지 않는다. 4 size 참고 평균: throughput **55.4%**(중앙값 41.2%), latency **0.39x**.
  참고 평균도 목표 85% 미달이며 개별 최소 75% 미달: 64, 256, 1024.
- `DEALER_ROUTER_REQREP` 대비 상대 비율(§2.1): 세 transport 모두 C++ 절대 처리량은 두 pattern이 40~62 Kops/s로
  비슷하고(`tcp` before와 같은 양상) 비율 차이는 C 쪽 값에서 나온다. 병목 후보로 기록할 만큼의 두 pattern 간
  격차는 없다.

### `MULTI_PUBSUB` (단순 one-way)

`tls`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 371,512.0 | 318,267.6 | 85.7% | 1735.007 ms | 1347.970 ms | 0.78x |
| 256 | 450,831.0 | 386,421.6 | 85.7% | 1625.040 ms | 1639.494 ms | 1.01x |
| 1024 | 570,731.8 | 510,275.6 | 89.4% | 1081.415 ms | 1279.027 ms | 1.18x |
| 4096 | 269,842.4 | 281,447.8 | 104.3% | 384.307 ms | 429.498 ms | 1.12x |
| 65536 | 24,928.8 | 33,832.2 | 135.7% | 250.834 ms | 186.928 ms | 0.75x |

- throughput ratio 산술평균 **100.2%**(중앙값 89.4%) — 기본 목표 95% 충족. 개별 최소 85% 미달 셀 없음(64·256B 85.7%는 경계).
- 평균 latency ratio 산술평균 **0.97x**(중앙값 1.01x) — 2.0x 상한 이내.

`ws`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 303,241.0 | 245,418.0 | 80.9% | 2160.379 ms | 2326.563 ms | 1.08x |
| 256 | 295,518.0 | 339,720.4 | 115.0% | 2103.648 ms | 1911.102 ms | 0.91x |
| 1024 | 472,641.6 | 508,585.0 | 107.6% | 1383.719 ms | 1438.375 ms | 1.04x |
| 4096 | 423,171.0 | 411,221.4 | 97.2% | 247.490 ms | 374.289 ms | 1.51x |
| 65536 | 37,616.8 | 44,993.4 | 119.6% | 273.661 ms | 244.485 ms | 0.89x |

- throughput ratio 산술평균 **104.1%**(중앙값 107.6%) — 기본 목표 95% 충족. 개별 최소 85% 미달: 64(80.9%, `tcp` 64B 80.9%와 같은 값).
- 평균 latency ratio 산술평균 **1.09x**(중앙값 1.04x) — 2.0x 상한 이내.

`wss`

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 425,867.2 | 355,878.8 | 83.6% | 1951.908 ms | 1948.999 ms | 1.00x |
| 256 | 488,905.2 | 438,128.6 | 89.6% | 1613.518 ms | 1625.871 ms | 1.01x |
| 1024 | 488,428.2 | 524,670.2 | 107.4% | 1237.367 ms | 1241.024 ms | 1.00x |
| 4096 | 217,927.2 | 254,271.6 | 116.7% | 633.061 ms | 450.503 ms | 0.71x |
| 65536 | 20,012.0 | 25,397.0 | 126.9% | 427.646 ms | 377.747 ms | 0.88x |

- throughput ratio 산술평균 **104.8%**(중앙값 107.4%) — 기본 목표 95% 충족. 개별 최소 85% 미달: 64(83.6%).
- 평균 latency ratio 산술평균 **0.92x**(중앙값 1.00x) — 2.0x 상한 이내.
- 세 transport 모두 두 runner의 평균 latency가 수백 ms~2초로 크다. C 기준도 같은 수준이므로 비율에는 영향이 작고
  진단값으로만 둔다(`tcp`와 동일).

## 판정 요약 (before — 세 transport 모두 개선 pass 전, 판정 미확정)

| Pattern | Transport | throughput aggregate | latency aggregate | 적용 목표 | 임시 상태 |
|---------|-----------|---------------------:|------------------:|-----------|-----------|
| `MULTI_DEALER_DEALER` | `tls` | 77.6% | 6.23x | 95% / 2.0x | `미달(77.6%)`, latency 상한 초과, 개선 pass 전 |
| `MULTI_DEALER_DEALER` | `ws` | 84.9% | 0.63x | 95% / 2.0x | `미달(84.9%)`, 개선 pass 전 |
| `MULTI_DEALER_DEALER` | `wss` | 93.9% | 1.91x | 95% / 2.0x | `미달(93.9%)`, 개선 pass 전 |
| `MULTI_DEALER_ROUTER_REQREP` | `tls` | 54.5% | 0.43x | 85% / 2.0x | `미달(54.5%)`, 개선 pass 전 |
| `MULTI_DEALER_ROUTER_REQREP` | `ws` | 미확정(4 size 참고 55.1%) | 참고 0.60x | 85% / 2.0x | 4096B `보류(C 기준 이상)`, 나머지 `미달`; Core/runner 조사 대기 |
| `MULTI_DEALER_ROUTER_REQREP` | `wss` | 미확정(4 size 참고 57.6%) | 참고 0.41x | 85% / 2.0x | 4096B `보류(C 기준 이상)`, 나머지 `미달`; Core/runner 조사 대기 |
| `MULTI_ROUTER_ROUTER_REQREP` | `tls` | 60.5% | 0.38x | 85% / 2.0x | `미달(60.5%)`, 개선 pass 전 |
| `MULTI_ROUTER_ROUTER_REQREP` | `ws` | 58.0% | 0.59x | 85% / 2.0x | `미달(58.0%)`, 개선 pass 전 |
| `MULTI_ROUTER_ROUTER_REQREP` | `wss` | 미확정(4 size 참고 55.4%) | 참고 0.39x | 85% / 2.0x | 4096B `보류(C 기준 이상)`, 나머지 `미달`; Core/runner 조사 대기 |
| `MULTI_PUBSUB` | `tls` | 100.2% | 0.97x | 95% / 2.0x | `통과 후보(100.2%)`, §7.4 14단계 검토 전 |
| `MULTI_PUBSUB` | `ws` | 104.1% | 1.09x | 95% / 2.0x | `통과 후보(104.1%)`, §7.4 14단계 검토 전 |
| `MULTI_PUBSUB` | `wss` | 104.8% | 0.92x | 95% / 2.0x | `통과 후보(104.8%)`, §7.4 14단계 검토 전 |

- 이 표의 상태는 모두 before 임시 상태다. 어느 transport도 §7.4 15~16단계의 완료·보류 기록으로 닫지 않았다.
- `MULTI_PUBSUB` 세 transport는 throughput·latency aggregate가 목표를 충족했지만, §7.4 14단계(aggregate가 목표를
  만족한 대상도 hot path와 POSDDD 리팩토링 요소를 한 번 검토하고 후보가 없으면 no-go 근거를 기록)와 §7.5의 가까운
  시점 C 재확인 조건을 아직 거치지 않았다. `통과 후보(비율%)`는 이 검토가 끝나면 `통과(비율%)`로 바꾸는 임시 표기이며
  §8의 최종 상태가 아니다. `tcp` `MULTI_PUBSUB`(93.3%, `미달`)의 개선 pass 결과가 나오면 같은 수신 경로 변경이 이 세
  transport에도 적용되므로 그 뒤 after를 한 번 재어 확정한다.
- `MULTI_DEALER_DEALER`는 `tcp` pass 1·pass 2를 거친 코드로도 세 transport 모두 95% 미달이다. `wss` 93.9%는 1.1%p
  차이지만 개선 pass 전에는 완화 목표 90%를 선택하지 않는다. `tls`는 latency aggregate 6.23x로 throughput과 latency가
  모두 미달이다.
- 두 REQREP pattern은 `tcp`와 같은 양상(64~4096B 37~54%, 65536B 96~113%)이며 `tcp`의 `보류` 근거(REQREP op당
  coroutine/scheduler/wrapper 고정비, public contract 유지 시 제거 대상 없음)가 그대로 적용될 가능성이 높다. 다만
  §7.4 16단계의 `보류`는 transport마다 자체 pass와 Sol 리뷰 pass의 결과를 기록한 뒤에만 쓸 수 있으므로 여기서는
  `미달`로 둔다.
- C 기준 이상 셀 세 개는 Core/runner 조사(brief `core-c-ws-reqrep-4k`)가 끝나 C 기준값이 다시 나올 때까지 비교를
  닫지 않는다. 조사 결과가 Core 변경이면 §5에 따라 새 Core release와 version을 확정한 뒤 C와 C++ 기준을 다시 만든다.

## 개선 후보 (`doc/perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md` §2 기준)

`tcp` before 로그의 후보 목록이 그대로 적용된다. transport별로 새로 관찰한 점만 적는다.

1. `MULTI_DEALER_DEALER` 소형 크기 wrapper 비용 — 세 transport 모두 64~256B 64~74%로 `tcp` pass 2 after(68.9~74.3%)와
   같은 수준이다. `tcp` pass 1·2가 즉시 admission 경로의 map/lock과 분리 allocation을 이미 제거했으므로, 남은 격차는
   transport와 무관한 message당 고정비다. `tls`·`wss`의 1024B~4096B latency outlier(719 ms, 655 ms, 214~216 ms)는
   C++ 송신 측 queue 깊이가 C보다 깊어진 상태를 뜻하므로 §2.3 completion drain과 WRITABLE 재제출 타이밍을 secure
   transport에서 다시 확인한다.
2. REQREP op당 비용 — `tcp` pass 2 callgrind(Ir/op 48.57k 대 C 23.98k)와 같은 고정비가 원인이며 transport별 새 병목
   신호는 없다. `tcp` `보류` 근거의 no-go 목록(scheduler `std::function` 변경, wrapper/Future/coroutine frame pool)이
   유효하다.
3. `MULTI_PUBSUB` 64B(80.9~85.7%) — `tcp` 64B 80.9%와 같은 셀이 세 transport에서도 가장 낮다. `tcp` `MULTI_PUBSUB`
   pass에서 §2.4 subscriber 수신 wrapper·native header 재사용을 점검한 결과를 공유한다.

public interface, ownership, error contract와 측정 의미를 바꾸는 후보(§5, 가이드 §4의 기각 후보)는 채택하지 않는다.
