# C++ Multi `tcp` 4 pattern before 측정 — Core 0.17.0

2026-09-05 03:49~03:54 KST에 C++ Multi suite의 `tcp` 첫 paired 측정을 실행했다. 이 기록은
개선 pass 전의 **before** 측정이며, 계획서 §9.1.2의 셀은 `미달(비율%)` 임시 상태로만
갱신했다. 완료 셀이 아니다.

## Manifest

| 항목 | 값 |
|------|----|
| source | `main` / `053a568ddd` (`docs(plan): refactoring pass done`), clean |
| Core runtime | `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` |
| Core build / version / revision | local `Release` + LTO / `0.17.0` / `053a568dddd04a8cb8efbc2f9ee9c0df915f4e47` (`core_dirty=0`) |
| binding | `zlink_cpp` 0.17.0, GCC/G++ 13.3.0 (`bindings/cpp/build` incremental) |
| host | WSL2 Linux `6.6.87.2-microsoft-standard-WSL2`, Intel i7-1260P, 16 logical CPUs, 11 GiB (`log/2026-09-05-environment.ko.md`) |
| CPU 상태 | governor 미노출, `--pin-cpu` 없음, 동시 perf process 없음 |
| pair tag | `p1cpp` |

Core runtime과 revision은 8개 report의 `META,core_runtime`, `META,core_revision`,
`META,core_dirty`에서 모두 같았다. 이 manifest의 Core artifact는 environment manifest의
`87057e8654` 시점 artifact가 아니라 `053a568ddd`에서 재링크한 artifact다.

## 명령

pattern마다 C를 먼저 실행하고 종료 직후 같은 조건으로 C++를 실행했다. 4 pattern을
`DEALER_DEALER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`, `PUBSUB` 순서로 순차 실행했다.

```bash
ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=4 PERF_MULTI_TIMEOUT_SECONDS=900 \
  bash bindings/c/perf/run_benchmarks_multi.sh --pattern <PATTERN> --transports tcp \
  --msg-sizes 64,256,1024,4096,65536 --clients 100 --duration 5 --runs 1 --results-tag p1cpp

ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$HOME/project/zlink/core/build \
  ZLINK_BUILD_JOBS=4 PERF_MULTI_TIMEOUT_SECONDS=900 \
  bash bindings/cpp/perf/run_benchmarks_multi.sh --pattern <PATTERN> --transports tcp \
  --msg-sizes 64,256,1024,4096,65536 --duration 5 --runs 1 --results-tag p1cpp
```

- C++ 명령은 `--clients`를 생략했지만 Effective Options의 `clients: 100`, `default_clients: 100`은 C와 같다.
- 이 세션의 로그에는 §7.1의 별도 smoke 실행 기록이 없다. 본 측정 report 자체가 모두 `status: complete`였다.
- 이번 실행은 §3.2의 6개 크기 중 `131072`를 제외한 5개 크기만 측정했다. `131072` 셀은 `미측정`으로 남긴다.

## 조건

| 항목 | 값 |
|------|----|
| suite / transport | Multi / `tcp` |
| pattern | `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`, `MULTI_PUBSUB` |
| size | 64, 256, 1024, 4096, 65536 |
| duration / runs | 5초 / 1회(`runs=1`이므로 측정값을 그대로 사용) |
| clients | 100 (`service_clients: auto`) |
| I/O threads | server 4, client 4 (default) |
| HWM | `auto-hwm`, `ctx_auto_hwm_profile: balanced`, `sndbuf/rcvbuf: -1` |
| timeout | `PERF_MULTI_TIMEOUT_SECONDS=900` |

## Report

모든 report는 `status: complete`, `expected_result_lines: 25`, `actual_result_lines: 25`였다.

| Pattern | C report (`bindings/c/perf/results/multi/report/`) | C++ report (`bindings/cpp/perf/results/multi/report/`) |
|---------|------|--------|
| `MULTI_DEALER_DEALER` | `perf_c_multi_linux_20260905_034919_p1cpp.txt` | `perf_cpp_multi_linux_20260905_035055_p1cpp.txt` |
| `MULTI_DEALER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_035123_p1cpp.txt` | `perf_cpp_multi_linux_20260905_035151_p1cpp.txt` |
| `MULTI_ROUTER_ROUTER_REQREP` | `perf_c_multi_linux_20260905_035218_p1cpp.txt` | `perf_cpp_multi_linux_20260905_035246_p1cpp.txt` |
| `MULTI_PUBSUB` | `perf_c_multi_linux_20260905_035313_p1cpp.txt` | `perf_cpp_multi_linux_20260905_035341_p1cpp.txt` |

실행 시각(`META,timestamp`, KST)과 시작 시점 load average(`META,load_avg`, 1/5/15분):

| 순서 | 실행 | 시각 | load_avg |
|------|------|------|----------|
| 1 | C `DEALER_DEALER` | 03:49:19 | 0.95 2.31 2.23 |
| 2 | C++ `DEALER_DEALER` | 03:50:55 | 1.13 2.01 2.13 |
| 3 | C `DEALER_ROUTER_REQREP` | 03:51:23 | 2.09 2.14 2.17 |
| 4 | C++ `DEALER_ROUTER_REQREP` | 03:51:51 | 2.98 2.36 2.24 |
| 5 | C `ROUTER_ROUTER_REQREP` | 03:52:18 | 5.29 3.01 2.46 |
| 6 | C++ `ROUTER_ROUTER_REQREP` | 03:52:46 | 4.85 3.11 2.51 |
| 7 | C `PUBSUB` | 03:53:13 | 5.80 3.55 2.68 |
| 8 | C++ `PUBSUB` | 03:53:41 | 6.00 3.78 2.78 |

1분 load average 2.7~6.2는 다른 작업이 아니라 순차 실행한 perf 실행 자체가 만든 값이다.
같은 pattern의 C와 C++는 27~96초 간격으로 연속 실행했다.

## Effective Options와 auto-HWM 대조

- 일치: `runs`, `patterns`, `transports`, `msg_sizes`, `duration_seconds`, `clients`,
  `default_clients`, `service_clients`, `server_io_threads`, `client_io_threads`, `hwm/sndhwm/rcvhwm`,
  `sndbuf/rcvbuf`, `ctx_auto_hwm_enable`, `ctx_auto_hwm_profile`, `stream_non_tcp_clients_max`,
  `timeout_seconds`.
- 표기만 다른 항목: C `routed_echo_per_socket_payload: none` / C++ `routed_echo_borrow_payload: none`,
  C `monitor_hwm_bytes: 4096000` / C++ `monitor_hwm: 4096000`.
- 값이 다른 항목: `default_stream_clients` C 100 / C++ 10000. `MULTI_STREAM`에만 쓰이는 값이라 이번
  4 pattern의 판정에는 영향이 없지만, `MULTI_STREAM` 측정 전에 맞춘다.
- auto-HWM detail의 `MsgUnit(B)`는 C와 C++ 모두 모든 행이 `?`로 출력됐다. 일치 여부를 값으로
  확인할 수 없어 진단 항목으로 남긴다. C++ `MULTI_DEALER_ROUTER_REQREP`와
  `MULTI_ROUTER_ROUTER_REQREP` report에는 `Auto-HWM detail` 블록 자체가 없다(C REQREP report에는 있다).
- effective SNDHWM/RCVHWM은 client 측이 모두 1048576(PUBSUB sub는 2097152)으로 같았고, server 측은
  C와 C++가 크기별로 1048576 또는 4096000으로 달랐다(`DEALER_DEALER` C server 전부 1048576,
  C++ server 1024B만 1048576; `PUBSUB` 256B C pub 1048576, C++ pub 4096000). auto-HWM이 실행 시점의
  memory budget으로 계산한 값이며 판정 gate는 아니지만 진단 자료로 기록한다.
- memory guard cap 발생 기록은 어느 report에도 없다.

## 측정값과 비율

비율은 `C++ / C`다. 판정 입력은 throughput ratio 산술평균과 평균 latency ratio 산술평균이며,
중앙값은 보조 자료다. p95/p99는 report에 있고 진단용으로만 둔다. 목표(§2.1/§2.2, C++):
단순 one-way `최소 85% / 중앙값 95%`, socket request/reply `최소 75% / 중앙값 85%`, 평균 latency 2.0x 이하.

### `MULTI_DEALER_DEALER` (단순 one-way)

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 1,079,498.4 | 605,522.0 | 56.1% | 0.519 ms | 0.163 ms | 0.31x |
| 256 | 974,458.6 | 586,876.2 | 60.2% | 2.039 ms | 0.191 ms | 0.09x |
| 1024 | 862,834.2 | 593,257.0 | 68.8% | 1.376 ms | 0.524 ms | 0.38x |
| 4096 | 358,081.8 | 292,841.6 | 81.8% | 649.123 ms | 822.232 ms | 1.27x |
| 65536 | 77,381.8 | 83,807.2 | 108.3% | 19.447 ms | 16.714 ms | 0.86x |

- throughput ratio 산술평균 **75.0%**(중앙값 68.8%) — 목표 95% 미달. 개별 최소 85% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.58x**(중앙값 0.38x) — 2.0x 상한 이내. 4096B는 1.27x로 개별 상한 이내.
- 4096B는 C와 C++ 모두 평균 latency가 649~822 ms로 다른 크기보다 세 자릿수 크다. 판정에는 쓰지 않고 진단값으로 남긴다.

### `MULTI_DEALER_ROUTER_REQREP` (socket request/reply)

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 155,527.2 | 63,233.8 | 40.7% | 0.963 ms | 0.772 ms | 0.80x |
| 256 | 149,064.2 | 64,462.8 | 43.2% | 0.984 ms | 0.738 ms | 0.75x |
| 1024 | 160,067.4 | 60,897.8 | 38.0% | 0.990 ms | 0.799 ms | 0.81x |
| 4096 | 128,155.6 | 58,332.0 | 45.5% | 1.263 ms | 0.822 ms | 0.65x |
| 65536 | 22,993.0 | 21,033.2 | 91.5% | 2.323 ms | 2.318 ms | 1.00x |

- throughput ratio 산술평균 **51.8%**(중앙값 43.2%) — 목표 85% 미달. 개별 최소 75% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.80x**(중앙값 0.80x) — 2.0x 상한 이내.

### `MULTI_ROUTER_ROUTER_REQREP` (socket request/reply)

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 139,506.2 | 62,543.8 | 44.8% | 0.960 ms | 0.769 ms | 0.80x |
| 256 | 128,620.4 | 58,917.2 | 45.8% | 1.078 ms | 0.817 ms | 0.76x |
| 1024 | 117,738.4 | 58,578.6 | 49.8% | 1.029 ms | 0.812 ms | 0.79x |
| 4096 | 103,833.8 | 55,796.4 | 53.7% | 1.314 ms | 0.853 ms | 0.65x |
| 65536 | 19,822.2 | 21,690.4 | 109.4% | 2.617 ms | 2.193 ms | 0.84x |

- throughput ratio 산술평균 **60.7%**(중앙값 49.8%) — 목표 85% 미달. 개별 최소 75% 미달: 64, 256, 1024, 4096.
- 평균 latency ratio 산술평균 **0.77x**(중앙값 0.79x) — 2.0x 상한 이내.
- `DEALER_ROUTER_REQREP` 대비 상대 비율(§2.1): C++ 절대 처리량은 두 pattern이 56~64 Kops/s로 비슷하고,
  C 쪽이 `ROUTER_ROUTER_REQREP`에서 더 낮아 비율이 4~9%p 높게 나온다.

### `MULTI_PUBSUB` (단순 one-way)

| Size | C throughput | C++ throughput | ratio | C latency | C++ latency | latency ratio |
|------|-------------:|---------------:|------:|----------:|------------:|--------------:|
| 64 | 626,918.2 | 507,287.8 | 80.9% | 1415.674 ms | 1387.553 ms | 0.98x |
| 256 | 590,575.4 | 641,879.8 | 108.7% | 1817.186 ms | 2083.996 ms | 1.15x |
| 1024 | 711,493.4 | 689,208.2 | 96.9% | 1148.044 ms | 1211.850 ms | 1.06x |
| 4096 | 655,174.8 | 566,639.4 | 86.5% | 352.786 ms | 433.349 ms | 1.23x |
| 65536 | 65,784.0 | 61,698.0 | 93.8% | 185.156 ms | 162.514 ms | 0.88x |

- throughput ratio 산술평균 **93.3%**(중앙값 93.8%) — 기본 목표 95% 미달. 개별 최소 85% 미달: 64.
  §2.1의 완화 목표 90%는 선택하지 않았다(개선 pass 전).
- 평균 latency ratio 산술평균 **1.06x**(중앙값 1.06x) — 2.0x 상한 이내.
- 두 runner 모두 평균 latency가 수백 ms~2초로 크다. C 기준도 같은 수준이므로 비율에는 영향이 작고 진단값으로만 둔다.

## 판정 요약 (before)

| Pattern | throughput aggregate | latency aggregate | 상태 |
|---------|---------------------:|------------------:|------|
| `MULTI_DEALER_DEALER` | 75.0% | 0.58x | `미달(75.0%)`, 개선 pass 전 |
| `MULTI_DEALER_ROUTER_REQREP` | 51.8% | 0.80x | `미달(51.8%)`, 개선 pass 전 |
| `MULTI_ROUTER_ROUTER_REQREP` | 60.7% | 0.77x | `미달(60.7%)`, 개선 pass 전 |
| `MULTI_PUBSUB` | 93.3% | 1.06x | `미달(93.3%)`, 개선 pass 전 |

네 pattern 모두 latency는 통과했고 throughput aggregate가 목표 미달이다. §7.4 9~11단계의
자체 hot-path 개선 pass와 Sol 리뷰 pass, 그 뒤의 after 측정이 남아 있다. 이 문서의 값은
before 기준이며, after 측정 시점에 host 부하가 달라졌으면 §7.3에 따라 C를 다시 측정한다.

## 개선 후보 (`doc/perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md` §2 기준)

아래는 가이드 §2 체크리스트가 가리키는 점검 후보다. 원인으로 확정한 것은 아니며, 프로파일과
allocation·copy 자료로 확인한 뒤 채택 여부를 정한다.

1. REQ/REP async request 경로의 op당 비용 (`MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`)
   - 관찰: C++는 64~4096B에서 56~64 Kops/s로 크기와 거의 무관하게 평평하고, C는 104~160 Kops/s다.
     65536B에서는 91.5~109.4%로 C와 같다.
   - 점검 항목: §2.1 즉시 성공 send에서 completion entry·Future·waiter map 등록이 토큰 반환 전에
     생기는지, 2-part staging이 heap을 쓰는지; §2.3 completion drain owner가 한 곳인지와 REQUEST
     completion·WRITABLE 분리, timeout-0 poll 재예약 여부; §2.4 server reply가 받은 `Message`를
     그대로 넘기는지(`Message → bytes → Message` 왕복 금지), 수신 wrapper·native header 재사용.
2. `MULTI_DEALER_DEALER` 소형 크기의 message당 wrapper 비용
   - 관찰: 64~1024B 56.1~68.8%, 4096B 81.8%, 65536B 108.3%로 크기가 커질수록 C에 가까워진다.
   - 점검 항목: §2.1 성공 send마다 completion entry·operation state를 만들지 않는지, payload 복사
     보관 여부, 2-part staging의 inline/stack 사용; §2.4 수신 wrapper 재사용과 payload 존재 여부
     캐시, 기본 wrapper 생성자의 native storage 초기화 여부.
3. `MULTI_PUBSUB` 64B(80.9%)와 4096B(86.5%)
   - 점검 항목: §2.4 subscriber 수신 wrapper·native header 재사용. aggregate가 95%에 1.7%p 못 미치므로
     수신 경로 점검 뒤에도 남으면 완화 목표 90% 선택 여부를 근거와 함께 기록한다.

public interface, ownership, error contract와 측정 의미를 바꾸는 후보(§5, 가이드 §4의 기각 후보)는
채택하지 않는다.
