# Round 115: STREAM ws 포함 guard

## 이번 라운드 목표

- round112 retained source 상태에서 STREAM 64B를 `tcp,tls,ws,wss` 전체로 다시 확인한다.
- May26 full 기준과 같은 transport 묶음에서 현재 수치가 어느 정도인지 본다.
- 이 라운드는 새 source 변경 후보가 아니라 retained 상태 검증이다.

## 기준 report

- May26 smoke:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round112 guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_173323_round112_pubsub_stream_guard_rerun.txt`

## POSD 검토

- source 변경 없음.
- round112의 판단 기준을 유지한다. output buffer 준비 정책은 `prepare_output_buffer()` 한 곳에 둔다.
- perf runner/client/server 조건은 수정하지 않는다.

## 실행 결과

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round115_stream_ws_inclusive_guard
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_175209_round115_stream_ws_inclusive_guard.txt`
- status: complete, fail 0.
- load_avg: `1.03 8.66 10.29`.
- clients: 10000.

| Transport | May26 smoke | May26 full | round112 guard | round115 |
|-----------|------------:|-----------:|---------------:|---------:|
| tcp | 325470.0 | 305177.4 | 324782.4 | 308062.4 |
| tls | 229781.0 | 214574.6 | 213598.6 | 201046.2 |
| ws | 263180.0 | 251311.4 | n/a | 248135.6 |
| wss | 200642.0 | 184722.2 | 182649.6 | 184299.0 |

## 최종 판단

- source 변경 없음.
- `tcp`, `ws`, `wss`는 May26 full 기준에 근접했다.
- `tls`는 round112 guard와 May26 full보다 낮게 나왔다. 새 변경으로 인한 하락은 아니지만,
  최종 채택 전 PUBSUB/STREAM guard에서 다시 확인해야 한다.
