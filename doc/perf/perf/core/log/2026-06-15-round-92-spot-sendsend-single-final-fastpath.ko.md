# Round 92: SPOT_SENDSEND 단일 FINAL part fast path

## 목적

`SPOT_SENDSEND/tcp,tls`가 May26 full 기준으로 계속 낮게 보였고, 서버 echo 경로는 수신한 단일
payload를 `zlink_spot_send_spot_part(..., ZLINK_PART_FINAL)`로 되돌려 보낸다. 기존 구현은 단일
FINAL part도 staged sequence 상태를 준비하고 vector로 옮긴 뒤 최종 submit을 호출했다.

이 round는 multipart가 아닌 단일 FINAL 전송에서 staged sequence 상태 머신을 건너뛰어도 계약과
소유권이 유지되는지 확인하고 성능을 측정했다.

## POSD 검토

- 기존 코드에는 단일 FINAL 메시지와 multipart sequence가 같은 staged 경로를 공유하는 시간적/상태
  비용이 있었다.
- 새 fast path는 공개 API를 늘리지 않고, 이미 존재하는 `zlink_send_part`/ROUTER 단일 FINAL fast path와
  같은 판단을 Spot send-spot에 좁게 적용한다.
- multipart sequence가 열려 있으면 기존 staged 경로를 그대로 사용한다.
- 실패 경로에서도 caller의 `part_`를 소비해 기존 staged 경로의 소유권 의미와 맞춘다.

## 변경

- `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()`에서 `part_flag_ == ZLINK_PART_FINAL`이고
    `send_sequence_active(spot_) == false`이면 `spot_send_spot_impl()`을 직접 호출한다.
  - backend 실패 시 `consume_send_part(part_)`를 호출해 기존 staged 경로처럼 caller part를 비운다.

## 검증

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure \
  -R 'test_spot_(dispatch_event|actor_dispatch|poller|router_channel_peer|runtime_activation|pubsub_scenario|service_introspection)|test_helper_(send_part_basic|more_bad_send|request_sequence_failure|interleave|ownership)|test_zmp_request_reply'
```

- build: pass
- focused CTest: 35/35 pass

### 소유권 확인

`spot_send_spot_impl()`은 `build_spot_routed_message()`를 호출한다. 실제 구현은
`core/src/runtime/services/spot/request_reply/spot_request_reply_local_dispatch.cpp`에 있으며,
payload part를 `zlink_msg_move()`로 combined 메시지에 옮긴다. header 생성 실패나 move 실패 시에도
남은 caller part를 `consume_send_frames_from()`으로 닫는다.

따라서 direct fast path가 성공하면 caller의 단일 `part_`는 기존 staged 경로와 같이 비워진다. 실패 시
fast path에서 `consume_send_part(part_)`를 호출해 기존 staged 경로의 소유권 의미와 맞춘다.

## perf: SPOT_SENDSEND

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern SPOT_SENDSEND \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round92_spot_sendsend_single_final_fastpath
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_150819_round92_spot_sendsend_single_final_fastpath.txt`
- status: complete
- success: 3
- fail: 0
- load_avg: `21.79 18.62 12.79`

| case | May26 full | May26 smoke | round90 current | round92 candidate | vs full | vs smoke | vs round90 |
|---|---:|---:|---:|---:|---:|---:|---:|
| SPOT_SENDSEND/tcp/64B | 271,206.0 | 264,042.0 | 245,644.8 | 262,496.8 | -3.21% | -0.59% | +6.86% |
| SPOT_SENDSEND/tls/64B | 254,009.6 | 247,003.0 | 233,617.4 | 241,624.0 | -4.88% | -2.18% | +3.43% |
| SPOT_SENDSEND/wss/64B | 252,557.8 | 277,203.0 | 248,696.6 | 253,583.8 | +0.41% | -8.52% | +1.97% |

## STREAM guard

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern STREAM \
  --transports tcp \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round92_spot_fastpath_stream_tcp_guard
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_151056_round92_spot_fastpath_stream_tcp_guard.txt`
- result: `STREAM/tcp/64B = 314,430.4 ops/s`

이 변경은 Spot send-spot API에만 닿고 STREAM 경로와 코드를 공유하지 않는다. 다만 STREAM/tcp는 여전히
측정 편차와 목표 미달이 남아 있으므로 별도 문제로 계속 추적한다.

## 결론

- `SPOT_SENDSEND` 세 전송은 모두 round90 current보다 올랐다.
- tcp는 +6.86%로 사용자가 말한 5% 개선 기준을 넘었다.
- tls/wss도 하락 없이 상승했다.
- POSD 관점에서도 단일 FINAL 케이스에서 불필요한 staged sequence를 생략하는 좁은 fast path라
  유지 후보로 본다.

## 다음

- 이 후보는 보존하고 다음 작업은 `STREAM/tcp` 목표와 `PUBSUB/tls` 반복 하락을 별도로 본다.
