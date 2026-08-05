# Round 93: STREAM packet complete-frame fast path 후보

## 목적

`STREAM/tcp/64B` 목표가 아직 400Kops에 못 미친다. packet handler 모드에서 64B echo는 대부분
prefix, header, body가 한 번에 들어오는 complete frame으로 보인다. 기존 parser는 complete frame도
prefix를 state buffer에 복사하고, state의 header/body 메시지를 만든 뒤 callback용 메시지로 move한다.

이 round에서는 partial packet state가 비어 있고 payload 안에 complete frame이 있을 때만 staging을
건너뛰는 fast path를 임시로 검토했다.

## POSD 검토

- 장점: complete frame hot path에서 prefix staging과 state message move를 줄일 수 있다.
- 위험: packet parser 안에 complete-frame 전용 분기가 늘어나며, partial frame과 maxmsgsize 처리의
  의미를 기존과 정확히 맞춰야 한다.
- 판단 기준: `STREAM/tcp`에서 분명한 개선이 없으면 parser 복잡도 증가를 남기지 않는다.

## 임시 변경

- `core/src/runtime/sockets/stream/stream.cpp`
  - `stream_dispatch_packet_msg_from_io()`에 complete frame fast path를 임시 추가했다.
  - partial state가 비어 있고 payload에 전체 frame이 있을 때만 직접 header/body 메시지를 만들어
    handler를 호출했다.
  - maxmsgsize 초과와 peer terminate 처리는 기존 경로와 같은 의미로 맞췄다.

## 검증

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure \
  -R 'test_multi_stream_server_reassembly|test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)'
```

- build: pass
- focused CTest: 20/20 pass

## perf

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round93_stream_packet_complete_frame_fastpath_tcp
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_151556_round93_stream_packet_complete_frame_fastpath_tcp.txt`
- status: complete
- success: 1
- fail: 0
- load_avg: `20.81 13.35 11.29`
- result: `STREAM/tcp/64B = 305,615.2 ops/s`

## 결론

- focused tests는 통과했지만 성능 근거가 없다.
- parser 내부 분기를 늘리는 변경이므로 POSD 기준상 유지하지 않는다.
- 후보는 되돌렸다.
- 되돌린 뒤 `cmake --build core/build -j$(nproc)`를 다시 실행해 runtime을 현재 source에 맞췄다.

## 다음

- `STREAM/tcp`는 packet parser의 complete-frame staging 제거만으로는 개선되지 않았다.
- 다음 후보는 packet parser보다 아래쪽인 ASIO read/write batching 또는 pipe wakeup 쪽을 다시 보되,
  HWM/flush/retry 계약은 변경하지 않는다.
