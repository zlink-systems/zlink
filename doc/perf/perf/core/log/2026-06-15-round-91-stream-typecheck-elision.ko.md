# Round 91: STREAM send 중복 타입 검사 제거 후보

## 목적

`zlink_send_part_rid()`의 STREAM 분기는 이미 socket type을 확인한 뒤 `send_stream_message()`로
들어간다. `send_stream_message()` 안의 `is_stream_type()` 검사가 중복인지 확인하고, 제거했을 때
`STREAM/tcp/64B`에 도움이 되는지 측정했다.

## POSD 검토

- 장점: 호출자가 이미 보장한 조건을 callee에서 다시 확인하지 않아 hot path 중복이 줄어든다.
- 위험: `send_stream_message()`는 이 파일 안에서 여러 경로가 공유하므로, 모든 호출자가 STREAM 타입을
  보장하는지 먼저 확인해야 한다.
- 판단: 모든 호출자는 `socket_type() == ZLINK_CORE_SOCKET_STREAM` 확인 뒤 호출한다. 다만 성능 효과가
  없으면 이 작은 구조 변경도 남기지 않는다.

## 임시 변경

- `core/src/api/socket/socket_message_send_api.cpp`
  - `send_stream_message()` 내부의 `is_stream_type(handle_)` 검사를 임시 제거했다.

## 검증

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure \
  -R 'test_stream_(socket|threadsafe|send_blocking_wakeup|fastpath|routing_id_size)|test_multi_stream_server_reassembly'
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
  --results-tag round91_stream_typecheck_elision_stream_tcp
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_150326_round91_stream_typecheck_elision_stream_tcp.txt`
- status: complete
- success: 1
- fail: 0
- load_avg: `18.90 10.79 8.64`
- result: `STREAM/tcp/64B = 303,085.2 ops/s`

## 결론

- round90 standalone current `328,506.4 ops/s`보다 낮다.
- 실행 load가 높았지만, 제거한 검사는 너무 작아 목표 개선 후보로 보기 어렵다.
- 후보는 되돌렸다.
- 되돌린 뒤 `cmake --build core/build -j$(nproc)`를 다시 실행해 runtime을 clean source에 맞췄다.

## 다음 판단

- public API 검증층의 단순 중복 제거만으로는 `STREAM/tcp` 목표에 가까워지지 않는다.
- 다음 후보는 STREAM echo의 실제 메시지 쓰기 경로, 또는 `SPOT_SENDSEND`와 공유되는 request/reply
  echo hot path에서 계약을 바꾸지 않는 중복 제거를 찾아야 한다.
