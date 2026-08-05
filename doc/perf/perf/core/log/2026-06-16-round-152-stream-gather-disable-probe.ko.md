# Round 152: STREAM gather disable probe

## 목표

- `MULTI_STREAM/tcp/64B`가 400kops 목표에 아직 못 미치므로 STREAM gather path의 64B 비용을 확인한다.
- perf runner/client/server는 수정하지 않는다.
- 먼저 환경 플래그로 진단하고, 하락 없는 명확한 근거가 있을 때만 core 기본값 변경을 검토한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- retained source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path

## 가설

- `prepare_gather_output()`은 STREAM gather가 켜져 있으면 64B처럼 gather 대상이 아닌 메시지도 먼저 꺼낸 뒤
  encoder에 넘기는 경로를 탄다.
- `ZLINK_ASIO_STREAM_DISABLE_GATHER=1`이 `STREAM/tcp/64B`에서 이 비용을 줄일 수 있는지 확인한다.

## POSD 검토

- 이 단계는 진단이다. public API, perf code, socket 계약은 수정하지 않는다.
- 만약 기본 정책을 바꾼다면 호출자에게 새 옵션을 요구하지 않고 transport/engine 내부 정책으로 흡수해야 한다.
- 단, 작은 개선과 다른 transport 하락이 섞이면 정책 의미가 모호해지므로 채택하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 라운드는 소스 변경이 없다.
- WS/WSS pending-copy 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서, decoder/message/send guard,
  `maxmsgsize` 정책을 수정하지 않았다.

## 진단 1: tcp 단독

```bash
ZLINK_ASIO_STREAM_DISABLE_GATHER=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round152_stream_tcp_disable_stream_gather_probe
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_061127_round152_stream_tcp_disable_stream_gather_probe.txt`
- 시작 load average: `0.65 1.88 2.23`
- `STREAM/tcp/64B`: 333,460.6 ops/s

인접 기본값:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round152_stream_tcp_default_adjacent
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_061139_round152_stream_tcp_default_adjacent.txt`
- 시작 load average: `0.71 1.84 2.21`
- `STREAM/tcp/64B`: 325,251.8 ops/s

tcp 단독 인접 비교는 gather 비활성화가 +2.52%였다.

## 진단 2: STREAM 전체 transport

```bash
ZLINK_ASIO_STREAM_DISABLE_GATHER=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round152_stream_all_disable_stream_gather_probe
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_061153_round152_stream_all_disable_stream_gather_probe.txt`
- 시작 load average: `0.60 1.78 2.18`
- success 4, fail 0

| case | disable gather |
|------|----------------|
| `STREAM/tcp` | 329,356.2 |
| `STREAM/tls` | 228,080.4 |
| `STREAM/ws` | 287,782.4 |
| `STREAM/wss` | 198,271.2 |

인접 기본값 전체:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round152_stream_all_default_adjacent
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_061248_round152_stream_all_default_adjacent.txt`
- 시작 load average: `2.67 2.13 2.28`
- status: partial
- `STREAM/tcp/64B`: 347,256.8 ops/s
- `STREAM/tls/64B`: 실패(`non_zero_exit_2_size_64`)

## 판정

- 소스 변경 없음.
- 기각한다.
- tcp 단독에서는 +2.52%였지만, 바로 뒤 STREAM 전체 기본값에서 tcp가 347,256.8 ops/s로 더 높게 나왔다.
- 전체 기본값 측정은 tls 실패 때문에 transport별 표본으로 쓰기 어렵지만, 최소한 tcp 목표 후보로는 일관된 개선을
  증명하지 못했다.
- `STREAM/tcp/64B`는 여전히 400kops 목표 미달이며, 이 후보는 채택하지 않는다.
