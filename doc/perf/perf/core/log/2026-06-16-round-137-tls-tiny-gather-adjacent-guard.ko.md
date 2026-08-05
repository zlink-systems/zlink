# Round 137: TLS tiny gather adjacent guard

## 목표

- round136에서 유지한 TLS tiny gather 후보가 `PUBSUB/tls` 외 패턴을 깎지 않는지 확인한다.
- full reduced 전 단계로 TLS transport 중심 guard를 먼저 실행한다.
- 하락 항목이 반복되면 round136 후보를 되돌린다.

## 시작 상태

- 유지 diff:
  - `SPOT_SENDSEND` 단일 FINAL fast path
  - SSL transport `async_writev()`와 encrypted tiny gather 128B fast path
- `git diff --check`: pass
- 직접 관련 CTest `tls|asio|pubsub|xpub|xsub`: 14/14 pass
- `test_discovery_resolve_spot`는 별도 타이밍성 실패가 남아 있다.

## guard 명령

```bash
sleep 45; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT_SENDSEND,STREAM \
  --transports tls \
  --duration 5 --runs 5 --connect-ready-timeout-ms 5000 \
  --results-tag round137_tls_tiny_gather_adjacent_tls_guard
```

## 결과

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_025344_round137_tls_tiny_gather_adjacent_tls_guard.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.42 1.67 2.09`
- 완료: success 6, fail 0

64B TLS throughput:

| pattern | current | vs May26 full | vs round122 | vs round134 |
|---------|---------|---------------|-------------|-------------|
| DEALER_DEALER | 3207835.2 | +4.72% | -0.75% | -0.29% |
| DEALER_ROUTER | 391897.4 | +2.67% | -1.73% | +1.47% |
| ROUTER_ROUTER | 378758.8 | -1.40% | -1.27% | +0.12% |
| PUBSUB | 2381032.4 | -9.23% | -2.36% | -0.06% |
| SPOT_SENDSEND | 252184.4 | -0.72% | +0.91% | +2.49% |
| STREAM | 228081.8 | +6.29% | -0.56% | +1.19% |

## 판단

- round134 대비 guard 하락은 `DEALER_DEALER -0.29%`, `PUBSUB -0.06%`로 noise 범위다.
- `PUBSUB/tls`는 May26 full 대비 여전히 낮지만 round134와 거의 같아 round136 후보의 부작용으로 보지 않는다.
- `SPOT_SENDSEND/tls`와 `STREAM/tls`는 round134 대비 상승했다.
- full reduced에서 다중 transport/패턴 순서 하락이 없으면 round136 후보를 유지한다.
