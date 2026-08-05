# Round 126: PUBSUB/STREAM low-load 재측정

## 이번 라운드 목표

- round125 low-load reduced full에서 목표를 누른 `PUBSUB/tcp,ws`와 `STREAM/wss`가
  source 후보 영향인지, 측정 창/순서 변동인지 분리한다.
- 이 라운드는 새 source 변경 없이 현재 round125 fast path 적용 runtime을 측정한다.

## 시작 상태

- 유지 중인 source 후보:
  - `zlink_spot_send_spot_part()` FINAL-only fast path.
- perf runner/client/server는 수정하지 않는다.
- `git diff --check` 통과.
- 시작 부하:
  - `load average: 1.54, 2.97, 3.63`

## 기준 report

- current reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_233726_round125_spot_sendsend_final_fastpath_lowload_all64_reduced_full.txt`
- previous clean current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_223125_round122_lowload_all64_reduced_full.txt`
- problem report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`

## 먼저 검증할 가설

- H1: 낮은 부하에서 `PUBSUB,STREAM`을 분리 실행하면 round125 reduced full의 `PUBSUB/tcp,ws`,
  `STREAM/wss` 하락이 완화되는가.
- H2: 완화되지 않으면 다음 source 후보는 PUBSUB fanout 또는 STREAM WSS transport 경로에서 고른다.

## POSD/보안 확인

- 새 source 변경 없음.
- WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard, maxmsgsize 정책을 수정하지 않는다.

## 실행 결과

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round126_pubsub_stream_lowload_recheck`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_001052_round126_pubsub_stream_lowload_recheck.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,2.65 3.10 3.65`
- status:
  partial, fail 1.
- failure:
  `MULTI_STREAM current wss 64B: non_zero_exit_2_size_64`

| pattern | transport | throughput |
|---------|-----------|-----------:|
| PUBSUB | tcp | 2531898.8 |
| PUBSUB | tls | 2340998.0 |
| PUBSUB | ws | 2200244.2 |
| PUBSUB | wss | 2610433.6 |
| STREAM | tcp | 264063.8 |
| STREAM | tls | 194371.0 |
| STREAM | ws | 218831.6 |
| STREAM | wss | fail |

## 판단

- `PUBSUB/ws`는 round125 reduced full `2053910.4`에서 `2200244.2`로 회복되어 round122 수준에 가깝다.
- `PUBSUB/tcp,tls`는 여전히 낮다.
- `STREAM`은 tcp/tls/ws도 낮고 wss는 실패했다.
- 다음 확인은 `STREAM/wss` 단독 실행이다. 단독도 실패하면 source 후보보다 transport 안정성/transition 문제로
  먼저 분리해야 한다.

## STREAM/wss 단독 재현

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round126_stream_wss_standalone_repro`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_001520_round126_stream_wss_standalone_repro.txt`
- runtime:
  `core/build/lib/libzlink.so.6.0.4`
- 시작 부하:
  `load_avg,4.14 3.47 3.65`
- status:
  complete, fail 0.
- result:
  `MULTI_STREAM/wss/64 = 173999.0`

## 추가 판단

- `STREAM/wss` 단독은 실패하지 않았다.
- `PUBSUB,STREAM` sequence에서만 실패했으므로, failure는 순수 단독 기능 실패보다 transition/order 영향이다.
- 성능 수치는 단독에서도 round122 `189070.4`보다 낮다.
- 다음 source 후보는 바로 넣기보다, `STREAM` 전용 low-load 반복과 transition 조건을 먼저 더 좁혀야 한다.
