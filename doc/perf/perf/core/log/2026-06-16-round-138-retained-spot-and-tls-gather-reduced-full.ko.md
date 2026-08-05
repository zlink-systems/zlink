# Round 138: retained SPOT and TLS gather reduced full

## 목표

- 현재 유지 중인 두 변경을 합산한 reduced full 결과를 확인한다.
- 64B 전체 패턴/transport에서 fail 0과 하락 항목 여부를 확인한다.

## 유지 변경

1. `SPOT_SENDSEND` 단일 FINAL fast path.
2. SSL transport `async_writev()`와 encrypted tiny gather 128B fast path.

## 사전 검증

- `git diff --check`: pass
- 직접 관련 CTest `tls|asio|pubsub|xpub|xsub`: 14/14 pass
- TLS adjacent guard: success 6, fail 0

## 실행 명령

```bash
sleep 45; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 --runs 5 --connect-ready-timeout-ms 5000 \
  --results-tag round138_retained_spot_tls_gather_reduced_full
```

## 결과

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_030017_round138_retained_spot_tls_gather_reduced_full.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.90 1.85 2.08`
- 완료: success 32, fail 0

요약:

| 기준 | count | avg | median | worst | best |
|------|-------|-----|--------|-------|------|
| May26 full | 32 | +1.29% | +1.93% | SPOT/wss -10.63% | STREAM/ws +8.97% |
| round122 | 32 | -0.90% | -0.70% | PUBSUB/tcp -5.50% | PUBSUB/ws +3.77% |
| round134 | 32 | -0.01% | +0.21% | STREAM/tls -3.76% | PUBSUB/ws +4.33% |

round134 대비 2% 이상 변동:

| pattern/transport | current | round134 | delta |
|-------------------|---------|----------|-------|
| ROUTER_ROUTER/ws | 405149.0 | 396346.2 | +2.22% |
| PUBSUB/ws | 2279958.6 | 2185363.2 | +4.33% |
| SPOT_REQREP/tls | 235369.0 | 240544.0 | -2.15% |
| SPOT_SENDSEND/ws | 238612.8 | 246330.4 | -3.13% |
| STREAM/tls | 216916.2 | 225391.8 | -3.76% |
| STREAM/wss | 191292.8 | 195207.6 | -2.01% |

STREAM 64B:

| transport | throughput |
|-----------|------------|
| tcp | 318827.8 |
| tls | 216916.2 |
| ws | 273842.8 |
| wss | 191292.8 |

## 판단

- fail 0은 충족했다.
- 전체 평균/중앙값은 May26 full 대비 플러스지만, round134 대비로는 사실상 보합이다.
- `STREAM/tls`가 round134 대비 -3.76%이고, round136 TLS tiny gather가 encrypted transport 전체에 적용되어
  STREAM/tls에도 들어간다.
- 이 후보는 그대로 유지하지 않는다. `encrypted tiny gather`를 non-STREAM encrypted path로 좁혀 STREAM 전용
  gather 정책과 분리한 뒤 재검증한다.
