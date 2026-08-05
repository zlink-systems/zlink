# Round 101: STREAM initial target cap probe

## goal

STREAM 64B echo에서 ASIO stream decoder/encoder 초기 target cap이 4096으로 시작하는 정책이 작은 메시지
latency와 transition run 처리량에 불리한지 확인한다.

완료 기준:

- source 변경 전 env probe로 `STREAM tcp,tls,wss 64B`를 확인한다.
- `tcp,tls,wss` 모두 하락 없이 개선되면 core policy 기본값 변경 후보로 검토한다.
- 한 항목이라도 하락하거나 실패하면 source 변경 없이 폐기한다.

## 기준

- round98 STREAM-only:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_154759_round98_stream_transport_transition_failure_recheck.txt`
- round100 reduced:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_155904_round100_reduced_stream_transition_stability.txt`

## 병목 가설

1. 64B STREAM echo는 많은 연결에서 작은 frame을 반복하므로 초기 4096B target이 cache/latency에 불리할 수
   있다.
2. target cap을 2048로 낮추면 작은 frame 처리에서는 좋아질 수 있지만 TLS/WSS batching에는 나빠질 수 있다.
3. source 기본값 변경은 전송별 하락이 없을 때만 검토한다.

먼저 검증할 가설:

- `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=2048` env probe가 STREAM tcp/tls/wss에서 하락 없이 개선되는지 본다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. 먼저 env probe만 수행한다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending-copy 제거, mtrie 비재귀화, port parsing, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.

## env probe

```bash
ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=2048 \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round101_stream_initial_target_cap_2048_probe
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_160755_round101_stream_initial_target_cap_2048_probe.txt`
- status: complete
- success: 3
- fail: 0
- start load_avg: `1.64 3.86 6.38`

| case | round98 default | round101 cap=2048 | delta |
|---|---:|---:|---:|
| STREAM/tcp/64B | 304,673.8 | 287,055.8 | -5.78% |
| STREAM/tls/64B | 185,889.0 | 213,381.4 | +14.79% |
| STREAM/wss/64B | 177,304.6 | 168,079.4 | -5.20% |

round100 reduced와 비교해도 `tcp`는 `280,166.0 -> 287,055.8`로 작게 오르지만, `wss`는
`175,863.2 -> 168,079.4`로 내려간다.

## 판정

- `tls` 단독으로는 좋아 보이지만 `tcp`와 `wss`가 내려간다.
- 특히 `wss` 하락이 반복 guard 조건을 만족하지 못한다.
- source 기본값 변경은 하지 않는다.
- 이 round는 env probe만 수행했고 source diff는 없다.
