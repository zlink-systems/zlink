# Round 160: STREAM/wss current refresh

## 목표

- round156 full64에서 `STREAM/wss`가 May26 full 대비 플러스지만 5% 미만이었으므로,
  단독 실행에서 현재 source 회귀인지 측정창 영향인지 분리한다.
- perf runner/client/server와 benchmark 조건은 수정하지 않는다.

## 기준

- May26 smoke:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round156 retained full64:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064431_round156_retained_spot_final_fastpath_full64_refresh.txt`

## 실행

```bash
cmake --build core/build --target libzlink -j$(nproc)
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports wss \
  --duration 5 --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round160_stream_wss_current_refresh
```

- build: 통과.
- runtime: `core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_074544_round160_stream_wss_current_refresh.txt`
- start load: `0.76 1.57 2.32`
- status: success `1`, fail `0`

## 결과

| 기준 | STREAM/wss 64B throughput | delta |
|------|--------------------------:|------:|
| May26 smoke | 200642.0 | +1.15% |
| May26 full | 184722.2 | +9.87% |
| round156 full64 | 191627.6 | +5.91% |
| round160 current | 202946.2 | 기준 |

## 판단

- `STREAM/wss`는 단독 current에서 May26 full 대비 거의 10% 상승했고,
  May26 smoke 대비도 플러스다.
- round156 full64에서 `+3.74%`에 그친 값은 긴 실행 순서 또는 load 영향으로 본다.
- `STREAM/wss`를 해결하려고 transport policy를 다시 건드릴 근거는 약하다.
- 새 source 변경은 하지 않는다.

## 보안 하드닝 보존 확인

- 이번 변경이 건드린 보안 항목:
  - 없음. build와 perf 재측정, 로그 추가만 수행했다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending-copy 제거, decoder/message/send guard, `maxmsgsize` 정책,
    mtrie 비재귀화, port parsing, IPC unlink 순서를 변경하지 않는다.
