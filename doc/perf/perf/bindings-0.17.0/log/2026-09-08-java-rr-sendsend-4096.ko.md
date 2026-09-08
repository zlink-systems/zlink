# Java RR SENDSEND 4096 B relay 실패 — 원인과 수정 (2026-09-08, 감독자 위임 Claude Fable)

고정 Core 0.17.2, tcp, 100 clients, `MULTI_ROUTER_ROUTER_SENDSEND` 4096 B에서 Java relay server가
결정적으로 실패했다(10:43·11:39 두 번 같은 위치). 64·256·1024 B는 435~445 Kops/s로 정상,
같은 relay를 쓰는 `MULTI_DEALER_ROUTER_SENDSEND` 4096 B는 통과, 65536 B는 실행 불가.

## 1. 실패 이유

실패 이유가 FAIL 줄에서 사라져 있었다. `PerfPolicy.sanitizeReason`
(`bindings/java/perf/common/src/main/java/systems/zlink/perf/PerfPolicy.java:33-48`)은 cause chain에서
**가장 깊은 non-blank message**만 남기는데, `ZlinkException`은 message가 `null`이다
(`bindings/java/src/main/java/systems/zlink/contracts/errors/ZlinkException.java:22-29`).
그래서 `IllegalStateException("multi routed relay failed", cause)`의 label만 남고
`SubmitResult`·errno가 통째로 버려졌다.

message에 result·errno를 붙이고 server log에 `RELAY_FAILURE_DETAIL` 한 줄을 찍은 뒤 재현했다
(ticket `2-1788835617-87377-claude-java`, rc=1):

```
RELAY_FAILURE_DETAIL,ZlinkSubmitException:result=BACKPRESSURED:errno=11
FAIL,current,ROUTER_ROUTER_SENDSEND,tcp,4096,multi_routed_relay_failed_zlinksubmitexception_result_backpressured_errno_11
```

`errno=11`은 `EAGAIN`이다. 원인은 relay가 **backpressure를 실패로 취급**한 것이다.

- relay는 reply를 `submit_sync()`로 보냈다
  (`PerfMultiRoutedRelay.java`, 수정 전 `submitReply` 82-99행).
- Java `submit_sync()`는 `SendFlags.NONE`으로 내려가는 blocking submit이다
  (`bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:148-153`,
  `requireSuccess`로 `SubmitResult != OK`면 즉시 예외).
- Core 규격상 `NONE FINAL`은 **호출 진입 시 `ZLINK_OPT_SNDTIMEO`를 snapshot하고 그때까지만 기다린다.
  기본값 1,000 ms이고 만료하면 `ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`으로 실패한다**
  (`core/doc/spec/core/socket/README.ko.md:988-991`). perf 러너는 `--sndtimeo`를 주지 않으므로
  (`PerfTransport.applySocketOptions:101-103`은 `sendTimeoutMs() >= 0`일 때만 설정) Core 기본 1,000 ms가 걸린다.
- 즉 4096 B·100 clients에서 echo reply가 1초 넘게 admission되지 못하는 정상적인 backpressure가
  그대로 hard failure가 됐다. 크기 의존성은 auto-HWM이 byte 단위라는 데서 온다(같은 SNDHWM에서 4096 B는
  64 B의 1/64 깊이). RR만 터진 것은 server→client 방향이 client마다 별도 route를 갖는 RR에서
  route당 credit이 먼저 마르기 때문이고, 코드에 크기 분기는 없다.
- 부수 피해: blocking submit이 recv 루프를 최대 1초 잡고 있으므로 relay가 그동안 아무 client도 받지 못한다.
  client 쪽 `multi_router_router_async_sends_timed_out`은 이 정지의 결과다.

C·C++·Node의 D-BP24 결함(무한 async reply → completion reservation 65,536 초과 → ENOMEM)과는 **다른 원인**이다.
Java는 애초에 sync였으므로 reservation을 쌓지 않았고, 대신 sync가 backpressure를 실패로 바꿨다.

## 2. 수정

`bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/`

- **새 파일 `PerfMultiRoutedReplyQueue.java`** — pending FIFO + 앞 admission을 기다리는 단일 sender.
  C relay의 `flush_pending_replies`/`try_send_reply_now`
  (`bindings/c/perf/multi/common/perf_multi_relay_server.hpp:331-431`)와 같은 구조다.
  C는 `DONTWAIT`로 제출하고 `EAGAIN`이면 immutable snapshot을 FIFO 앞에 남겼다가 WRITABLE 토큰으로 재시도한다.
  Java는 public async `submit()`이 그 재시도를 binding 안에서 소유하므로
  (`CompletionOwner.submitSend:101-144` — backpressure면 parts를 retain하고 WRITABLE 재시도 후 stage를 완료),
  FIFO는 "앞 reply가 admission될 때까지 다음 reply를 제출하지 않는다"만 담당한다.
  - stale route(`NOT_CONNECTED`/`NOT_FOUND`/`ENOTCONN`/`EHOSTUNREACH`)는 그 한 건만 버리고 FIFO 머리를 비운다.
    C의 `reply_send_stale_route` 분기와 같다.
  - 즉시 admission은 Core가 stage를 inline으로 완료하므로 re-entrancy 가드로 바깥 loop 하나가 계속 돌게 했다
    (reply당 스택 프레임 금지).
- **`PerfMultiRoutedRelay.java`** — `submit_sync()` → async `submit()`. 받은 것은 `Message.from` 스냅샷으로
  FIFO에 넣으므로 `received`는 다음 recv에 그대로 재사용된다.
  STOP 뒤에는 정책 drain 창(`PERF_MULTI_SEND_DRAIN_TIMEOUT_MS`, 기본 5,000 ms)만큼 남은 reply의 admission을
  기다린다. C의 종료 drain(`perf_multi_relay_server.hpp:527-600`)과 같은 자리다.
- **수신-송신 결합** — reply가 admission을 기다리는 동안에는 Core 수신 queue에서 다음 request를 꺼내지 않는다
  (`PerfMultiRoutedRelay.drainRequests`의 `replies.awaitIdle(50)`). C도 wait token이 살아 있으면 다음 reply를
  제출하지 않는다(`perf_multi_relay_server.hpp:200-201`). 이 결합이 없으면 relay가 Core의 bounded 수신 queue를
  무한한 application FIFO로 옮겨 담아 Core backpressure가 무력화된다. 실제로 결합 없이 돌린 중간 판에서
  4096 B는 client RESULT는 나왔지만 relay가 STOP 뒤 backlog를 5,000 ms drain 창 안에 비우지 못해
  `server_exit_124`로 죽었다. 숫자 상한이 아니라 "앞 것을 Core 송신 queue에 넘기기 전에는 다음 것을
  수신 queue에서 꺼내지 않는다"는 결합 규칙이다.
- **진단 영구화** — `describe()`가 `ZlinkSubmitException:result=…:errno=…`를 만들고,
  relay 실패는 `RELAY_FAILURE_DETAIL`을, drain 창 초과는 `RELAY_DRAIN_DETAIL`을 server log에 남긴다.
  같은 `describe()`를 client 쪽 `PerfMultiTargetCoordinator.throwIfFailed`와
  `PerfMultiAsyncSendLoop.rethrow`에도 붙여서 client FAIL 줄에서도 result·errno가 살아남는다.

## 3. 불변 준수

- timeout·sleep·client 수·메시지 크기·상한 숫자를 바꾸지 않았다. `--sndtimeo`도 손대지 않았다.
- 인위적 in-flight cap 없음(D-BP15). FIFO는 무한이고 상한 숫자가 없다. 동시 미admission 1건은
  "앞 것이 admission되기 전에는 다음 것을 제출하지 않는다"는 C의 결합 구조 그 자체이며,
  app이 고른 window가 아니라 Core admission이 페이스를 정한다. 부수 효과로 completion reservation은
  socket당 최대 1이므로 D-BP24의 65,536 초과 경로에도 들어가지 않는다.
- 오류를 삼키지 않았다. 오히려 이전에 사라지던 result·errno를 FAIL 줄과 server log에 드러냈다.
  기존의 "STOP 이후 submit 실패는 무시" 규칙만 그대로 유지했다(teardown race).

## 4. 검증

- 단위 테스트 `PerfMultiRoutedReplyQueueTest` 5건 신규 + 기존 10건 = `:perf-multi:test` 15/15 통과.
  회귀 커버: FIFO 순서, 앞 admission 전 다음 제출 금지, 깊은 FIFO의 inline admission 비재귀 소진,
  **backpressure는 실패가 아니다**, stale route는 그 한 건만 버리고 진행, terminal 실패는 sender 정지 + 잔여 해제.
- perf(모두 ticket 경유, 고정 Core 0.17.2, tcp, 100 clients, duration 5):
  - **① `MULTI_ROUTER_ROUTER_SENDSEND` tcp 64·256·1024·4096·65536 B — `status: complete`, `fail: 0` (2회)**
    - 12:19 `perf_java_multi_linux_20260908_121935_javarr.txt`:
      468 K(64) · 441 K(256) · 457 K(1024) · **145 K(4096)** · 44.5 K(65536) ops/s
    - 12:21 `perf_java_multi_linux_20260908_122135_javarr4.txt`:
      468 K · 459 K · 430 K · **124 K** · 22.2 K ops/s
    - 수정 전에는 4096 B가 결정적 FAIL, 65536 B는 실행 자체가 불가였다.
  - **② `MULTI_DEALER_ROUTER_SENDSEND` tcp 4096 B 3-run — `status: complete`, `fail: 0` (2회)**
    - 12:20 `..._122017_javadr.txt` 250,155 ops/s / 12:21 `..._122110_javadr5.txt` 244,748 ops/s
    - 수정 전 동일 셀 1-run 274,375 ops/s(10:21). C 기준 ~348~406 K에 대해 수정 전 78.7% → 지금 ~70~72%.
      내 실행 구간은 perf 큐가 계속 차 있었고 같은 판 안의 편차도 컸으므로(203~250 K) 회귀로 단정하지 않는다.
      **미해결 항목으로 남긴다** — 한가한 구간에서 수정 전/후를 같은 조건으로 재측정해야 판정할 수 있다.
  - relay server log에 `RELAY_FAILURE_DETAIL`·`RELAY_DRAIN_DETAIL` 없음(모든 셀).

## 5. 남은 것

- **4096 B의 절대 성능**: RR 4096 B는 124~145 K ops/s로 C(~395 K)의 31~37%다. latency mean 641 ms,
  p99 1,259 ms. 같은 4096 B에서 DR도 latency mean 614~697 ms로 수정 전부터 같은 수준이었으므로
  4096 B의 큐 깊이 자체가 이 harness의 특성으로 보이지만, RR의 처리량 격차는 별도 항목이다.
- 이 수정은 SENDSEND relay(RR·DR 공용)만 다룬다. REQREP 경로
  (`2026-09-08-java-reqrep-64k-analysis.ko.md`)는 별개 항목이다.
- `PerfPolicy.sanitizeReason`이 message 없는 typed 예외를 다루지 못하는 구조 자체는 남아 있다.
  이번에는 호출부에서 detail을 붙여 우회했다.
