# Round 153: STREAM tiny gather and current pipe recheck

## 목표

- `MULTI_STREAM/tcp/64B`의 현재 retained 상태에서 이전에 봤던 STREAM hot path 후보를 재검토한다.
- perf runner/client/server는 수정하지 않는다.
- 하락 항목이 있거나 개선이 반복되지 않으면 소스에 남기지 않는다.

## 기준

- retained source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path
- 직전 STREAM/tcp 참고값:
  - `round152_stream_tcp_default_adjacent`: 325,251.8 ops/s
  - `round152_stream_all_default_adjacent`: 347,256.8 ops/s, 단 tls 실패로 partial

## 후보 A: 64B tiny gather

```bash
ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD=64 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round153_stream_tcp_tiny_gather64_probe
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_061403_round153_stream_tcp_tiny_gather64_probe.txt`
- 시작 load average: `1.71 2.04 2.24`
- `STREAM/tcp/64B`: 321,496.4 ops/s

판정:

- 기각.
- 64B를 gather write로 강제하는 쪽은 직전 기본값보다 낮았다.

## 후보 B: current pipe 단일 조회

적용한 후보:

- `stream_dispatch_send_current_msg_from_io()`에서 `current_pipe()`를 직접 한 번 조회하고,
  같은 포인터에서 peer를 계산하도록 바꿨다.
- public API와 stream 계약은 수정하지 않았다.

기능 검증:

```bash
cmake --build core/build --target libzlink -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_(stream_socket|stream_fastpath|transport_matrix|multi_socket_contract_regressions)$'
```

- build: 통과
- CTest: 4/4 통과

perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round153_stream_current_pipe_single_lookup_candidate
```

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_061504_round153_stream_current_pipe_single_lookup_candidate.txt`
- 시작 load average: `2.33 2.11 2.25`
- `STREAM/tcp/64B`: 317,013.8 ops/s

판정:

- 기각하고 되돌렸다.
- 코드상 중복 조회를 줄이는 후보였지만, hot path 측정에서 개선이 없었다.
- POSD 관점에서도 작은 내부 정리는 가능하지만, 이 작업의 목적은 성능 개선이며 검증된 이득이 없으므로 유지하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 후보 B는 `stream_dispatch_send_current_msg_from_io()` 내부 조회 방식만 바꿨다가 되돌렸다.
- 최종 source에는 이 라운드 후보가 남지 않았다.
- WS/WSS pending-copy 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서, decoder/message/send guard,
  `maxmsgsize` 정책을 수정하지 않았다.

## 최종 상태

- 소스 변경 없음.
- `core/build`는 되돌린 최종 소스로 다시 빌드했다.
