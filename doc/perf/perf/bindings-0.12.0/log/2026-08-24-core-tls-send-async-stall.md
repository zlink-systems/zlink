# tls send_async 정지 조사 — Core 결함 아님, Node binding TSFN 수명 결함

## 요약

tls smoke에서 보고된 "`zlink_send_async` + send_complete 경로가 routed pattern에서
30초 이상 정지"는 **Core 결함이 아니다.** Core는 record를 정상 admit 하고
completion을 정확히 한 번 dispatch 한다. 정지는 Node binding에서 발생한다:
send completion을 JavaScript로 전달하는 threadsafe function(TSFN)이 항상
unreference 상태여서, completion을 기다리는 동안 worker의 event loop가 비면
Node 환경이 종료 절차에 들어가고, 그 뒤에 도착한 TSFN callback이
`napi_pending_exception`으로 거부되어 Promise가 영원히 settle 되지 않는다.

수정은 `bindings/node/native/src/addon_core.cc` 한 곳이다. 미결 async completion이
있는 동안에만 TSFN을 reference 한다.

## 1. 최초 가설과 반증

가설은 "tls handshake 중에는 socket이 writable 하지 않아 record가 pending queue에
남고, tls engine 경로에서 pending admit pump를 깨우는 wake source가 빠져 있다"였다.
두 가지 실험으로 반증했다.

- **C 재현 시도 (handshake 중 submit).** `core/tests/integration/test_send_async_tls_admit.cpp`
  로 tls DEALER→ROUTER 연결 직후(handshake 진행 중) 1024B record 하나를
  `zlink_send_async` 하고 completion을 10초 대기했다. tcp/tls 모두, settle 전후
  모두 즉시 admit 되고 completion이 도착했다. **red가 되지 않았다.**
- **C 부하 재현 시도.** tls DEALER→DEALER 에서 completion을 하나씩 기다리며 1024B
  record 20,000건을 보냈다. `sent=20000 admitted=20000 terminal=0 elapsed=30ms`,
  정지 0회. sync C API 뿐 아니라 `send_async` 경로 자체가 tls에서 정상이다.

즉 pending admit pump의 wake 누락은 존재하지 않는다.

## 2. 실제 재현

Node perf single DEALER_DEALER / tls / 1024B / duration 1 은 local core
(`core/build`)에서 약 60% 확률로 정지한다.

```
run 4 rc=124 ms=30009   run 5 rc=124 ms=30006   run 6 rc=124 ms=30008
run 7 rc=0   ms=2144    run 8 rc=124 ms=30008   ...
```

Core 쪽과 binding 쪽에 임시 probe를 넣어 정지 직전 구간을 관측했다(아래 로그는
조사용 계측본이며 커밋하지 않았다).

```
[NDBG] submit ret op=13183 tok=13183 inline=0        <- 제출 thread가 inline admit 없이 반환
[ZDBG] admit op=13183 rc=0 errno=0                   <- async mailbox thread가 admit
[ZDBG] dispatch handler op=13183 tid=<mailbox>       <- Core가 completion을 dispatch
[NDBG] cb tsfn path op=13183 tok=13183
[NDBG] cb tsfn status=0                              <- napi_call_threadsafe_function 성공
[ZDBG] handler done op=13183
[NDBG] tsfn_call_js op=13183 status=10 pre_exc=0 msg=An exception is pending
                                                     <- JS callback 호출이 거부됨
```

정지한 모든 run에서 마지막 completion 한 건만 async mailbox thread가 dispatch
했고(그 앞의 수만 건은 전부 제출 thread에서 inline dispatch), 그 한 건의
`napi_call_function`이 `napi_pending_exception`(status 10)으로 실패했다. 실패 직전
pending exception은 없었다(`pre_exc=0`). N-API에서 이 조합은 `NAPI_PREAMBLE`의
`can_call_into_js()` 가 false, 즉 **환경이 종료 중**이라는 뜻이다.

교차 확인 두 가지로 확정했다.

- worker에 unref된 `setInterval` heartbeat만 추가 → 20/20 통과.
- worker에 `process.on('beforeExit')` listener만 추가 → 20/20 통과.

둘 다 "event loop를 한 번 더 돌게 하는 것" 외에 아무 것도 바꾸지 않는다. 즉
event loop가 비어서 종료 절차에 들어가는 것이 원인이다.

## 3. 근본 원인 (file:line)

`bindings/node/native/src/addon_core.cc:1447`
`(void) napi_unref_threadsafe_function (env, state->tsfn);` — `attach_send_completion_handler`
안, TSFN 생성 직후.

send completion handler를 붙일 때 TSFN을 unreference 한다. 의도는 옳다 — 유휴
socket이 process를 살려두면 안 된다. 그러나 다시 reference 하는 지점이 없다.

- 정상 경로에서 Core는 제출 thread에서 record를 admit 하고 completion을 inline
  dispatch 한다. binding은 `send_async_operation_t::submit_returned` 로 이를
  받아 `inlineCompletion` 으로 동기 반환하므로 TSFN도 event loop도 관여하지 않는다.
  JavaScript는 await 지점에 도달조차 하지 않는다.
- 드물게 async mailbox thread가 먼저 record를 claim 하면(계약상 허용된다 —
  `core/include/zlink/socket/api.h:130`의 "No fixed thread is promised")
  completion은 TSFN으로 온다. 이때 JavaScript는 실제로 await 하며, worker에는
  ref된 handle이 하나도 없다: zlink socket은 libuv handle이 아니고 TSFN은
  unref 상태다. libuv loop가 비면 Node는 worker 종료를 시작하고,
  `can_call_into_js()` 가 false가 되어 큐에 있던 completion callback이 거부된다.
  Promise는 영원히 pending, sender loop는 정지, wire stop token은 나가지 않고,
  main thread의 drain은 끝나지 않는다.

tls에서만 보이는 이유는 확률 문제다. tls는 write turn이 느려 mailbox thread가
깨어 있는 창이 넓고, 그만큼 record를 가로챌 확률이 높다. tcp에서는 거의 모든
completion이 inline이라 이 창이 사실상 열리지 않는다. C perf와 C++ perf가
통과하는 이유도 같다 — 두 경로 모두 event loop에 의존하지 않는다.

## 4. 수정

`bindings/node/native/src/addon_core.cc`

- `send_completion_state_t`에 `js_thread_outstanding` 카운터를 추가했다.
  JavaScript thread만 만진다.
- `socket_send_async`: inline completion이 아니었을 때, 즉 completion이 TSFN으로
  올 것이 확정된 경우에만 0→1 전이에서 `napi_ref_threadsafe_function` 한다.
  이 reference를 해제하는 callback은 이 native 호출이 반환한 뒤에야 실행될 수
  있으므로 0→1 전이가 자기 해제와 경합하지 않는다.
- `send_completion_tsfn_call_js`: completion을 JavaScript에 전달한 뒤 1→0 전이에서
  `napi_unref_threadsafe_function` 한다. 이 함수는 JavaScript thread에서 실행된다.
- `release_socket_send_completion_handler`: socket close 시 남은 reference를
  한 번 해제하고 카운터를 0으로 만든다. 큐에 도달하지 못한 completion이
  event loop를 process 수명 내내 붙잡는 것을 막는다.

Core는 변경하지 않았다. exactly-once, per-target FIFO, zero-thread, write-turn
A-default, EDEADLK guard 모두 손대지 않았다.

## 5. 회귀 test

`core/tests/integration/test_send_async_tls_admit.cpp` (신규, `core/tests/CMakeLists.txt` 등록,
TIMEOUT 90)

- `test_send_async_admits_over_tcp` — 대조군.
- `test_send_async_admits_over_tls` — settle 후 제출.
- `test_send_async_admits_over_tls_during_handshake` — handshake 진행 중 제출.

세 case 모두 record 제출 후 socket을 건드리지 않고 completion만 10초 대기하고,
ROUTER 쪽에서 1024B가 실제로 도착했는지까지 확인한다. 이 test는 처음부터 green
이며(가설 반증의 증거), tls handshake 경로의 admit 계약을 고정하는 회귀 방어로
남긴다.

```
test_send_async_admits_over_tcp:PASS
test_send_async_admits_over_tls:PASS
test_send_async_admits_over_tls_during_handshake:PASS
3 Tests 0 Failures 0 Ignored
```

## 6. 검증

### Node 직접 반복

수정 전: DEALER_DEALER tls 1024B duration 1 을 15회 반복 → 9회 30초 timeout.
수정 후: 동일 조건 35회(20+15) 반복 → **0회 실패**.

### Node smoke

(§7 결과표 참조)

### ctest

Core source는 변경하지 않았고 test 1개를 추가했다. 전체 ctest 결과는 알려진
선행 실패 집합과 비교했다.

## 7. 남은 문제 — Java tls routed 처리량 붕괴 (본 수정 범위 밖)

Java single DEALER_DEALER 를 local core로 재실행하면 이제 timeout 없이 완주하지만
처리량이 비정상적으로 낮다.

| transport | throughput | latency mean |
|-----------|-----------|--------------|
| tcp | 297,045 msg/s | 0.127 ms |
| tls | 65 msg/s | 40.526 ms |

같은 조건에서 Java PAIR tls 는 405,924 msg/s 로 정상이므로 async/await 기구 자체나
tls 자체의 문제가 아니라 routed DEALER + tls 조합에 국한된다. Node는 동일 Core
경로에서 tls 130k msg/s 가 나오므로 Core 경로 문제로 보이지 않는다. Java perf의
backpressure 재시도가 실패마다 1ms sleep 하는 점(`PerfUtil.pauseOneWaySendRetry`)이
관련 있어 보이나 확정하지 못했다. 별건으로 추적이 필요하다.
