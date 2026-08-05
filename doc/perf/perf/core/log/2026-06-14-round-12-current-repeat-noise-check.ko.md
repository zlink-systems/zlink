# Round 12: 현재 런타임 반복 측정과 noise 확인

- 기준 커밋: `5e3c438a2`
- source 변경: 없음
- 목적: round 10, round 11에서 transport별 결과가 크게 갈린 원인이 실제 source 효과인지
  단일-run noise인지 확인한다.

## 실행

```bash
PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB,SPOT \
  --transports tcp,tls,ws,wss \
  --msg-sizes 64 \
  --duration 5 \
  --runs 2 \
  --results-tag round12_repeat_current
```

- 결과 파일: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_171324_round12_repeat_current.txt`
- success 8, fail 0
- runtime: `core/build/lib/libzlink.so.6.0.4`

## 관찰

- `MULTI_PUBSUB tcp`는 run 1이 2.482 Mmsg/s, run 2가 2.222 Mmsg/s로 약 10% 차이가 났다.
- `MULTI_SPOT tcp`는 run 1이 3.287 Mmsg/s, run 2가 3.897 Mmsg/s로 더 큰 차이가 났다.
- 따라서 단일-run에서 보이는 ±5% 수준의 변화는 후보 유지 근거로 쓰기 어렵다.

## zero-fail 기준 대비 median 비교

비교 기준은 `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`이다.

| 패턴 | 전송 | 변화 |
|------|------|------|
| `MULTI_PUBSUB` | tcp | -1.74% |
| `MULTI_PUBSUB` | tls | -0.18% |
| `MULTI_PUBSUB` | ws | -2.83% |
| `MULTI_PUBSUB` | wss | +0.20% |
| `MULTI_SPOT` | tcp | +2.37% |
| `MULTI_SPOT` | tls | +0.92% |
| `MULTI_SPOT` | ws | -9.48% |
| `MULTI_SPOT` | wss | +4.06% |

## 판정

- PUBSUB 64B는 현재 median 기준으로 zero-fail 기준과 거의 같은 범위에 있다.
- SPOT 64B도 ws를 제외하면 zero-fail 기준과 같거나 더 높다.
- round 11의 `SPOT wss +19.16%`와 `SPOT tls -14.86%`는 source 효과라기보다 단일-run
  variance로 보는 것이 맞다.

## 후속 기준

- 이후 64B PUBSUB/SPOT 후보는 최소 `--runs 2` median에서 10% 이상 개선될 때만 source를
  유지한다.
- 단일-run targeted perf는 failure 0 확인과 큰 회귀 탐지 용도로만 사용한다.
