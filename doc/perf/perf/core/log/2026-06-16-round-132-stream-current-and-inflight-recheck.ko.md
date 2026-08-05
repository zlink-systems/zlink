# Round 132: STREAM current and inflight counter recheck

## 목표

- round131에서 채택한 SPOT_SENDSEND fast path를 유지한 상태로 STREAM 64B 현재 수치를 다시 확인한다.
- 이전 round113에서 높은 부하 때문에 배제한 `_dispatch_inflight` 제거 후보를 낮은 부하에서 재검토한다.

## 기준

- corrected smoke baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- corrected full baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round122 low-load STREAM:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_222903_round122_stream_tls_ws_wss_rerun.txt`

## STREAM current

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round132_stream_current_after_spot_retained`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_013012_round132_stream_current_after_spot_retained.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,0.49 1.46 2.80`
- status: complete, fail 0

| transport | current kops | May26 full | full delta | May26 smoke | smoke delta |
|---|---:|---:|---:|---:|---:|
| tcp | 306093.4 | 305177.4 | +0.30% | 325470.0 | -5.96% |
| tls | 225763.2 | 214574.6 | +5.21% | 229781.0 | -1.75% |
| ws | 286993.8 | 251311.4 | +14.20% | 263180.0 | +9.05% |
| wss | 191913.0 | 184722.2 | +3.89% | 200642.0 | -4.35% |

## 판단

- corrected May26 full 기준으로는 STREAM 64B가 모두 하락 없이 플러스다.
- tcp는 400kops 목표에는 아직 멀지만, "현재 250kops" 수준의 하락은 이번 낮은 부하 창에서는 재현되지 않았다.
- corrected smoke 기준으로 보면 tcp와 wss가 낮으므로, 목표 달성을 위해서는 tcp 중심의 추가 개선 후보가 필요하다.

## 재검토 후보

- 후보:
  STREAM dispatch callback hot path의 `_dispatch_inflight` atomic counter 제거.
- 이유:
  현재 검색 기준으로 `_dispatch_inflight` 값은 internal virtual getter 외에 판단 경로에서 읽히지 않는다.
  hot path에서는 raw/packet callback마다 atomic add/sub만 수행한다.
- POSD 검토:
  죽은 상태를 제거해 내부 상태를 줄인다. public API, wire format, callback signature, perf runner는 수정하지 않는다.
  `stream_dispatch_in_callback()` 기반 lifecycle guard는 유지한다.

## 후보 검증

### 기능 검증

- `git diff --check`: 통과
- `cmake --build core/build --target libzlink -j$(nproc)`: 통과
- `ctest --test-dir core/build --output-on-failure -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly'`
  - 20/20 통과

### 성능 검증

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round132_stream_inflight_removed_candidate`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_013430_round132_stream_inflight_removed_candidate.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- start load:
  `load_avg,2.22 2.27 2.82`
- status: complete, fail 0

| transport | current before | inflight removed | delta |
|---|---:|---:|---:|
| tcp | 306093.4 | 298756.0 | -2.40% |
| tls | 225763.2 | 216505.8 | -4.10% |
| ws | 286993.8 | 248166.8 | -13.53% |
| wss | 191913.0 | 175337.0 | -8.64% |

## 최종 판단

- 미채택.
- 후보 run의 시작 부하가 current run보다 높았지만 네 전송 모두 낮고, 특히 ws/wss 하락 폭이 크다.
- 사용자가 정한 "하락 항목 없이 플러스" 기준을 만족하지 못한다.
- source 변경은 되돌렸다.
- 원복 후 `cmake --build core/build --target libzlink -j$(nproc)`를 다시 실행해 perf runtime을 원래 상태로 갱신했다.
