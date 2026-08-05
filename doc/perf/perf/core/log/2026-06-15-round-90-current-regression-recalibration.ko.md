# Round 90: 현재 clean source 재측정으로 남은 하락 항목 보정

## 목적

이전 후보들이 대부분 작은 개선 또는 일부 하락을 함께 만들었기 때문에, 코드 변경 없이 현재
clean source의 남은 하락 항목을 다시 측정했다. 목적은 실제 반복 하락과 실행 편차를 분리하고,
작은 개선 후보를 다시 볼 때의 채택 기준을 명확히 하는 것이다.

사용자가 기준을 May26 smoke/full refresh 결과로 정정했으므로 아래 두 파일을 비교 기준으로 쓴다.

- `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`

참고 현재값은 round70 reduced full refresh다.

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`

## POSD 기준

- 작은 개선이라도 다른 항목 하락이 없고 구현 복잡도가 늘지 않으면 후보로 볼 수 있다.
- 반대로 상태, 캐시, 특수 분기를 늘리는 변경은 1-2% 개선만으로 남기지 않는다.
- HWM, retry, flush, callback 직렬화 의미를 바꾸는 변경은 성능 후보가 아니라 계약 변경으로 본다.
- 측정 편차가 큰 항목은 단독 결과로 채택하지 않고 반복성과 하락 부재를 같이 확인한다.

## 실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB,SPOT_SENDSEND,STREAM \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round90_current_regression_recalibration
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_145417_round90_current_regression_recalibration.txt`
- status: complete
- success: 9
- fail: 0
- load_avg: `5.00 9.33 9.11`

STREAM/tcp는 전체 실행 순서 영향이 있어 단독으로 한 번 더 측정했다.

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round90_stream_tcp_standalone_recheck
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_150006_round90_stream_tcp_standalone_recheck.txt`
- status: complete
- success: 1
- fail: 0
- load_avg: `2.02 4.34 6.85`

## 결과

| case | May26 full | May26 smoke | round70 | round90 | round90 vs full | round90 vs smoke | round90 vs round70 |
|---|---:|---:|---:|---:|---:|---:|---:|
| PUBSUB/tcp/64B | 2,661,635.6 | 2,677,051.0 | 2,468,643.4 | 2,548,363.4 | -4.26% | -4.81% | +3.23% |
| PUBSUB/tls/64B | 2,623,065.0 | 2,537,614.0 | 2,264,552.0 | 2,278,477.4 | -13.14% | -10.21% | +0.61% |
| PUBSUB/wss/64B | 2,760,571.0 | 2,529,036.0 | 2,505,671.8 | 2,494,252.0 | -9.65% | -1.38% | -0.46% |
| SPOT_SENDSEND/tcp/64B | 271,206.0 | 264,042.0 | 251,575.8 | 245,644.8 | -9.43% | -6.97% | -2.36% |
| SPOT_SENDSEND/tls/64B | 254,009.6 | 247,003.0 | 234,358.8 | 233,617.4 | -8.03% | -5.42% | -0.32% |
| SPOT_SENDSEND/wss/64B | 252,557.8 | 277,203.0 | 252,167.8 | 248,696.6 | -1.53% | -10.28% | -1.38% |
| STREAM/tcp/64B | 305,177.4 | 325,470.0 | 332,250.4 | 320,253.2 | +4.94% | -1.60% | -3.61% |
| STREAM/tls/64B | 214,574.6 | 229,781.0 | 217,262.2 | 219,801.0 | +2.44% | -4.34% | +1.17% |
| STREAM/wss/64B | 184,722.2 | 200,642.0 | 177,889.2 | 190,692.6 | +3.23% | -4.96% | +7.20% |

STREAM/tcp 단독 재측정:

| case | result | vs May26 full | vs May26 smoke | vs round70 |
|---|---:|---:|---:|---:|
| STREAM/tcp/64B standalone | 328,506.4 | +7.64% | +0.93% | -1.13% |

## 해석

- `STREAM/tcp/64B`는 May26 full 기준으로는 이미 높은 편이지만, smoke/round70보다 낮고 400Kops
  목표에는 아직 부족하다. 단독 실행에서도 328.5Kops라서 실행 순서 편차만으로 400Kops에 가까워지지는 않는다.
- `PUBSUB/tls/64B`는 May26 full/smoke 양쪽 대비 10% 이상 낮아 반복 하락으로 계속 본다.
- `PUBSUB/wss/64B`는 May26 full 대비로는 낮지만 smoke 대비로는 -1.38%다. 기준 파일 선택 영향이 크다.
- `SPOT_SENDSEND/tcp/tls`는 May26 full 대비 각각 -9.43%, -8.03%로 아직 주의 항목이다.
- `SPOT_SENDSEND/wss`는 full 대비 -1.53%지만 smoke 대비 -10.28%다. smoke/full 편차가 커서 단독
  판단하지 않는다.

## 다음 판단

- 과거 1-2% 후보는 하락 항목이 없는지 다시 볼 수 있다. 다만 `STREAM/tcp`, `PUBSUB/tls`,
  `SPOT_SENDSEND/tcp/tls`를 함께 확인해야 한다.
- `STREAM/tcp`는 callback current-rid fast path가 이미 들어가 있으므로, 단순 route lookup 제거
  후보는 반복하지 않는다.
- `pipe_t` HWM, retry, flush 의미를 약화하는 변경은 성능 수치가 좋아도 POSD와 공개 계약 관점에서
  채택하지 않는다.
- 다음 후보는 public API 검증층 또는 STREAM callback fast path에서 상태를 늘리지 않는 작은 중복
  제거로 제한한다.
