# Round 95: SPOT_REQREP reply 단일 FINAL part fast path 검토

## 목적

`SPOT_REQREP` 서버 echo 경로는 받은 단일 payload를
`zlink_spot_reply_spot_part(..., ZLINK_PART_FINAL)`로 되돌려 보낸다. `SPOT_SENDSEND`에서 단일 FINAL
fast path가 효과를 보였기 때문에, 같은 staged sequence 비용을 reply 경로에서도 줄일 수 있는지
검토했다.

## POSD 검토

- 공개 API나 호출자 계약은 바꾸지 않는다.
- multipart sequence가 열려 있으면 기존 staged 경로를 유지한다.
- 단일 FINAL이고 열린 sequence가 없을 때만 기존 submit API인 `spot_reply_spot_impl()`을 직접 호출한다.
- request 쪽은 pending state, callback, timeout 소유권이 더 복잡하므로 건드리지 않았다.

## 소유권 확인

`spot_reply_spot_impl()`은 `build_spot_request_reply_message()`를 호출한다. 이 함수는
`core/src/runtime/services/spot/request_reply/spot_request_reply_local_dispatch.cpp`에서 payload part를
`zlink_msg_move()`로 combined 메시지에 옮긴다. header 생성 실패나 move 실패 시에도 남은 caller part를
`consume_send_frames_from()`으로 닫는다.

따라서 단일 FINAL direct path 자체는 staged path와 같은 소유권 의미를 만들 수 있다. 하지만 성능 결과가
혼합되어 최종 변경으로 남기지 않았다.

## 현재 기준 측정

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern SPOT_REQREP \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round95_spot_reqrep_current_before_reply_fastpath
```

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_152357_round95_spot_reqrep_current_before_reply_fastpath.txt`
- status: complete
- success: 3
- fail: 0

| case | current before |
|---|---:|
| SPOT_REQREP/tcp/64B | 253,153.0 |
| SPOT_REQREP/tls/64B | 229,534.8 |
| SPOT_REQREP/wss/64B | 217,464.0 |

## 후보 측정 1

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern SPOT_REQREP \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round95_spot_reqrep_reply_single_final_fastpath
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_152803_round95_spot_reqrep_reply_single_final_fastpath.txt`
- status: complete
- success: 3
- fail: 0
- start load_avg: `20.81 13.24 11.00`

| case | current before | candidate | delta |
|---|---:|---:|---:|
| SPOT_REQREP/tcp/64B | 253,153.0 | 252,588.6 | -0.22% |
| SPOT_REQREP/tls/64B | 229,534.8 | 239,557.4 | +4.37% |
| SPOT_REQREP/wss/64B | 217,464.0 | 210,140.6 | -3.37% |

## 후보 측정 2

첫 후보 측정은 시작 load가 높았기 때문에 낮은 load에서 재실행했다.

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern SPOT_REQREP \
  --transports tcp,tls,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round95_spot_reqrep_reply_single_final_fastpath_retry
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_153042_round95_spot_reqrep_reply_single_final_fastpath_retry.txt`
- status: complete
- success: 3
- fail: 0
- start load_avg: `3.00 8.44 9.54`

| case | current before | candidate retry | delta |
|---|---:|---:|---:|
| SPOT_REQREP/tcp/64B | 253,153.0 | 253,768.2 | +0.24% |
| SPOT_REQREP/tls/64B | 229,534.8 | 232,971.4 | +1.50% |
| SPOT_REQREP/wss/64B | 217,464.0 | 216,359.0 | -0.51% |

## 검증

후보 적용 상태에서 실행했다.

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure \
  -R 'test_spot_(dispatch_event|actor_dispatch|poller|router_channel_peer|runtime_activation|pubsub_scenario|service_introspection)|test_helper_(send_part_basic|more_bad_send|request_sequence_failure|interleave|ownership)|test_zmp_request_reply'
```

- build: pass
- focused CTest: 35/35 pass

후보 폐기 후 `cmake --build core/build -j$(nproc)`를 다시 실행해 `core/build` runtime을 현재 소스와
맞췄다.

## 결론

- POSD와 소유권 관점에서는 좁은 변경으로 볼 수 있었지만, perf가 채택 기준을 만족하지 않았다.
- 재실행에서도 `wss`가 `-0.51%` 내려갔다.
- 사용자와 맞춘 기준인 "하락 항목 없이 작게라도 개선"에 맞지 않으므로 후보를 폐기했다.
- 최종 소스에는 이 reply fast path를 남기지 않았다.
