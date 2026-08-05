# Round 13: DEALER/STREAM 현재 런타임 반복 측정

- 기준 커밋: `5e3c438a2`
- source 변경: 없음
- 목적: PUBSUB/SPOT 외에 남은 64B gap이 현재 zero-fail 기준 대비 실제로 남아 있는지 확인한다.

## 실행

처음 실행은 `--msg-sizes 64`가 runner 출력에 반영되지 않아 중단했다. 재실행은
`PERF_MSG_SIZES=64` 환경변수로 크기를 고정했다.

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern DEALER_DEALER,STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 2 \
  --results-tag round13_dealer_stream_64_repeat_current
```

- 결과 파일: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_171942_round13_dealer_stream_64_repeat_current.txt`
- success 8, fail 0
- runtime: `core/build/lib/libzlink.so.6.0.4`

## zero-fail 기준 대비

비교 기준은 `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`이다.

| 패턴 | 전송 | 변화 |
|------|------|------|
| `MULTI_DEALER_DEALER` | tcp | -0.27% |
| `MULTI_DEALER_DEALER` | tls | -1.53% |
| `MULTI_DEALER_DEALER` | ws | -2.12% |
| `MULTI_DEALER_DEALER` | wss | -0.09% |
| `MULTI_STREAM` | tcp | +7.98% |
| `MULTI_STREAM` | tls | +5.32% |
| `MULTI_STREAM` | ws | +7.63% |
| `MULTI_STREAM` | wss | +6.34% |

## 장기 기준 대비

비교 기준은 `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`이다.

| 패턴 | 전송 | 변화 |
|------|------|------|
| `MULTI_DEALER_DEALER` | tcp | -25.58% |
| `MULTI_DEALER_DEALER` | tls | -25.69% |
| `MULTI_DEALER_DEALER` | ws | -25.42% |
| `MULTI_DEALER_DEALER` | wss | -24.69% |
| `MULTI_STREAM` | tcp | -14.33% |
| `MULTI_STREAM` | tls | -4.69% |
| `MULTI_STREAM` | ws | -20.21% |
| `MULTI_STREAM` | wss | -5.54% |

## 판정

- 현재 zero-fail 기준 대비 DEALER/STREAM의 추가 하락은 확인되지 않았다.
- STREAM은 오히려 현재 반복 측정에서 zero-fail 기준보다 높다.
- 장기 기준 대비 DEALER 약 -25%는 별도 장기 회귀 후보로 남는다.

## 후속

- 단기 목표는 failure 0 유지와 현재 zero-fail 기준 대비 회귀 제거로 보면 대부분 정리됐다.
- 의미 있는 추가 개선 후보는 장기 기준 대비 DEALER 하락 원인을 별도 bisect 또는 설계 변경 이력으로
  추적해야 한다.
