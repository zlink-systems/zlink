# Round 96: STREAM tiny gather-write threshold probe

## 목적

`STREAM/tcp/64B`는 May26 full 기준보다 높게 나온 적은 있지만 400Kops 목표에는 아직 멀다.
ASIO STREAM 출력 경로는 gather-write를 기본 켜두지만, 작은 payload는
`ZLINK_ASIO_STREAM_GATHER_THRESHOLD`보다 작으면 encoder batch copy 경로를 탄다. 이 round에서는
소스 변경 전에 `ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD=128`로 64B packet frame도 gather-write를
사용하게 했을 때 신호가 있는지 확인했다.

## POSD 검토

- 새 API나 새 상태를 추가하지 않고 기존 정책 env만 사용했다.
- 소스 기본값을 바꾸려면 tcp뿐 아니라 tls/ws/wss에서 하락이나 실패가 없어야 한다.
- 작은 payload에서 writev가 copy보다 항상 낫다는 보장은 없으므로, 전체 전송 결과가 혼합되면
  기본값 변경은 하지 않는다.

## 현재 guard

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round96_stream_tcp_current_guard_after_round95_revert
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153518_round96_stream_tcp_current_guard_after_round95_revert.txt`
- start load_avg: `12.40 12.63 10.83`
- result: `STREAM/tcp/64B = 295,355.8 ops/s`

같은 낮은 load에서 no-env를 다시 측정했다.

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round96_stream_tcp_current_guard_after_tiny_gather_probe
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153701_round96_stream_tcp_current_guard_after_tiny_gather_probe.txt`
- start load_avg: `3.13 9.01 9.71`
- result: `STREAM/tcp/64B = 304,056.6 ops/s`

## tiny gather tcp probe

```bash
ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD=128 \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round96_stream_tcp_tiny_gather_128_probe
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153649_round96_stream_tcp_tiny_gather_128_probe.txt`
- start load_avg: `3.17 9.32 9.82`
- result: `STREAM/tcp/64B = 309,391.0 ops/s`
- vs near no-env: `+1.75%`

## tiny gather all-transport probe

```bash
ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD=128 \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 3 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round96_stream_tiny_gather_128_all_probe
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153719_round96_stream_tiny_gather_128_all_probe.txt`
- status: partial
- success: 2
- fail: 2
- start load_avg: `2.57 8.60 9.56`

| case | result |
|---|---:|
| STREAM/tcp/64B | 301,452.2 |
| STREAM/tls/64B | 199,892.2 |
| STREAM/ws/64B | fail |
| STREAM/wss/64B | fail |

## 결론

- tcp 단독에서는 `+1.75%` 신호가 있었지만, 전체 전송에서는 tcp도 낮고 tls가 크게 낮았다.
- ws/wss는 partial 실패가 발생했다.
- 하락 없는 후보가 아니므로 소스 기본값을 바꾸지 않는다.
- 이 round는 probe만 수행했고 source diff는 없다.
