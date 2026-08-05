# Round 116: PUBSUB/STREAM ws 포함 통합 guard

## 이번 라운드 목표

- round112 retained source 상태에서 `PUBSUB,STREAM` 64B를 `tcp,tls,ws,wss` 전체로 확인한다.
- STREAM 개선이 PUBSUB 또는 다른 transport 하락을 동반하는지 본다.
- 이 라운드는 새 source 변경 후보가 아니라 retained 상태 검증이다.

## 기준 report

- May26 smoke:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round112 guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_173323_round112_pubsub_stream_guard_rerun.txt`
- round115 STREAM-only guard:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_175209_round115_stream_ws_inclusive_guard.txt`

## POSD 검토

- source 변경 없음.
- round112 retained source는 ASIO output buffer 준비 정책을 한 helper로 모으는 변경만 남긴다.
- perf runner/client/server 조건은 수정하지 않는다.

## 실행 결과

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB,STREAM --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round116_pubsub_stream_ws_inclusive_guard
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_175322_round116_pubsub_stream_ws_inclusive_guard.txt`
- status: complete, fail 0.
- load_avg: `2.24 7.31 9.68`.
- effective clients: 100.

| Pattern | Transport | round112 guard | round116 |
|---------|-----------|---------------:|---------:|
| PUBSUB | tcp | 2433742.6 | 2410578.2 |
| PUBSUB | tls | 2230630.8 | 2157448.2 |
| PUBSUB | ws | n/a | 2011579.6 |
| PUBSUB | wss | 2470511.8 | 2426659.4 |
| STREAM | tcp | 324782.4 | 270713.6 |
| STREAM | tls | 213598.6 | 168558.4 |
| STREAM | ws | n/a | 220631.4 |
| STREAM | wss | 182649.6 | 163289.2 |

## 최종 판단

- source 변경 없음.
- 실패는 없었지만 STREAM `tcp/tls/wss`가 round112 guard보다 낮게 나왔다.
- 같은 source의 round115 STREAM-only guard는 `tcp=308062.4`, `ws=248135.6`,
  `wss=184299.0`으로 May26 full에 가까웠다.
- 따라서 이 결과는 패턴 전환 또는 실행 환경 편차 가능성이 있지만, 최종 채택 전 STREAM-only 재확인이 필요하다.
