# Java SENDSEND 4096 B teardown 상호 대기 — 계측·원인·수정 (2026-09-08, 감독자 위임 Claude Fable)

고정 Core `0.17.3-alpha` prefix(`libzlink.so.0.17.2`), tcp, 100 clients, duration 5.
`MULTI_ROUTER_ROUTER_SENDSEND` 4096 B가 간헐적으로(태그 `javadrain` 1/4) client 쪽
`FAIL,...,multi_router_router_async_sends_timed_out`으로 죽었다. 같은 빌드의 `javarr`·`javarr4`·
`javadrain2`는 complete였다. relay server log에는 `RELAY_DRAIN_DETAIL,timed_out,pending=0,sending=true`가
남아 있었다.

## 1. 계측 — 무엇이 남았나

teardown 진입/이탈에 client stderr 한 줄을 넣고(`CLIENT_DRAIN_DETAIL`) `--msg-sizes 4096 --runs 5`로
재현했다(ticket `2-1788842291-53809-claude-java-rrss`, rc=1,
report `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260908_133839_javarrdiagteardown.txt`,
`status: partial`, `MULTI_ROUTER_ROUTER_SENDSEND tcp 4096B: client_exit_1`).

실패 run의 client log(`bindings/java/perf/results/multi/tmp/router_router_sendsend_tcp_4096_client.log`):

```
CLIENT_DRAIN_DETAIL,enter,pending=59,submitted=1376898,received=693976,outstanding=682922,...
CLIENT_DRAIN_DETAIL,exit,pending=2,elapsed_ms=15002,...
FAIL,current,ROUTER_ROUTER_SENDSEND,tcp,4096,multi_router_router_async_sends_timed_out
```

같은 run의 server log:

```
RELAY_DRAIN_DETAIL,timed_out,pending=0,sending=true
```

- **양쪽 다 남았다.** client는 teardown 진입 시 100개 중 59개 socket이 send admission pending이었고,
  15,002 ms(= `max(5 s, duration×3 s)`) 창을 다 쓰고도 2개가 끝나지 않았다.
- 동시에 relay는 FIFO가 비었는데(`pending=0`) 마지막 reply 하나의 admission이 끝나지 않은 채
  (`sending=true`) 5,000 ms 창을 다 썼다.
- 미수신 echo 682,922건 — client 수신 queue가 HWM(4096 B에서 1,048,576 B ÷ 4096 = route당 256건)까지
  차 있었다는 뜻이다.

## 2. 원인 (파일:줄)

세 지점이 맞물린 상호 대기다.

1. `PerfMultiRoutedSendCoordinator.java`(class `PerfMultiTargetCoordinator`)의 teardown이
   `admissions.awaitLatest(terminalTimeout, label)` → `PerfMultiAsyncSendLoop.awaitAll`
   (`PerfMultiAsyncSendLoop.java:38-55`)로 **수신을 멈추고 `CompletableFuture.allOf().get()`에 블록**했다.
   active 구간에서는 같은 loop가 poll(0)으로 계속 수신했지만, 창이 끝나는 순간 수신이 끊긴다.
2. client 수신 queue가 HWM에서 막히면 relay의 마지막 reply가 Core admission을 받지 못한다.
3. relay는 앞 reply가 admission될 때까지 다음 request를 수신 queue에서 꺼내지 않는다
   (`PerfMultiRoutedRelay.drainRequests`의 `replies.awaitIdle(50L)` — 2026-09-08 relay 수정에서
   의도적으로 넣은 C 대응 결합, `perf_multi_relay_server.hpp:200-201`).
   그래서 relay가 멈추면 client의 send admission도 끝나지 않는다.

→ client는 admission을 기다리며 수신을 멈추고, relay는 그 수신이 없어서 admission을 못 하고,
그래서 client의 admission도 안 끝난다. 4096 B에서만 터지는 것은 auto-HWM이 byte 단위여서
route당 큐 깊이가 64 B의 1/64이기 때문이고, 간헐적인 것은 active 종료 시점에 "수신 큐가 꽉 참 +
send admission이 pending"이 동시에 성립해야 하기 때문이다.

## 3. 수정과 C 대응

`bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/`

- **`PerfMultiRoutedSendCoordinator.java`** — `AdmissionRoundRobin.awaitLatestWhileReceiving()` 신설.
  teardown 창 동안 `poll(≤50 ms)` → 준비된 socket에서 echo 수신 → 반복하며 send terminal을 기다린다.
  창이 끝나는 조건은 **두 쪽이 다 정리됐을 때**다: send terminal이 전부 완료 **그리고**
  이미 admission된 request가 갚아야 할 echo를 다 받았을 때.
  C의 drain loop 조건 `tracker_has_retained_sends (tracker) || tracker_has_pending_replies (tracker)`
  (`bindings/c/perf/multi/common/perf_multi_client_helpers.hpp:1367-1391`)과 같은 형태다.
  C는 `service_events (poll_rc, false)`로 창 안에서 계속 수신하되 metric은 세지 않는다
  (`같은 파일:1247-1249`의 `!count_metrics || !inside_active_deadline` → `continue`).
  .NET은 `PerfMultiEchoReplyDrain.WaitAsync`
  (`bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiAdmissionSignal.cs:118-151`)가
  같은 구조다 — pending이 남아 있으면 `poll(timeoutMs)` + `dispatch`를 반복한다.
  - 갚아야 할 echo 수는 **admission이 성공한 건수**로만 센다(terminal callback에서 `admittedCount`
    증가). C가 `slot->replies`를 성공한 send에서만 올리는 것과 같다. 거부된 admission은 echo를
    빚지지 않으므로 창을 헛되이 쓰지 않는다.
  - send terminal이 실패로 확정되면(`hasFailure()`) 즉시 창을 빠져나가 원래대로 실패를 올린다.
  - 창의 길이·정책은 그대로다. loop가 끝난 뒤에도 `PerfMultiAsyncSendLoop.awaitAll`을 남은 시간으로
    호출해 terminal 상태(정책 timeout 문구 포함)를 그대로 다시 낸다.
- **`PerfMultiRouterRouter.java` / `PerfMultiDealerRouter.java`** — teardown 전용 drainer
  `drainRepliesForTeardown` 추가. active 창이 끝난 뒤의 reply는 **받아서 버린다**(RESULT 집계에 넣지
  않는다). 정책 `PERF_MULTI_TEST_POLICY.md` §12.3의 "이 drain은 새 제출을 하지 않으며 RESULT 집계를
  늘리지 않는다"를 그대로 지킨다. 기존 active용 `drainReplies`는 로직 변경 없이 소비 건수만 돌려주도록
  반환형만 `void` → `int`로 바꿨다.
- **진단 영구화** — `CLIENT_DRAIN_DETAIL,enter/exit`에 pending admission 수, submitted·admitted·
  received, 남은 echo, 소요 ms를 남긴다. relay의 `RELAY_DRAIN_DETAIL`과 짝이 된다.

### 중간판에서 드러난 두 번째 지점 (기록)

수신만 계속하고 **send terminal이 끝나는 즉시** 창을 닫은 1차 판(`javarrfix`,
`perf_java_multi_linux_20260908_134208_javarrfix.txt`)에서는 client는 26 ms 만에 정상 종료했지만
(`pending=0,drained=3877`), 미수신 echo 686,805건을 그대로 두고 나가는 바람에 relay의 마지막 reply가
계속 admission되지 못했고, STOP 뒤 relay가 자기 drain 창 5,000 ms를 다 쓰다가 runner의
`SERVER_SHUTDOWN_TIMEOUT_MS`(5,000 ms)에 걸려 `server_exit_124`로 죽었다(run 3/5).
즉 **echo를 다 받고 나가는 것까지가 C 모델**이고, 그래서 최종판은 owed echo까지 창의 종료 조건에 넣었다.
실측으로 이 추가 drain은 RR 1,502 ms / DR 2,059 ms이며(아래 log 인용), 창(15,000 ms) 안이다.

## 4. 불변 준수

- **active 구간 무변경.** `PerfMultiTargetCoordinator.run`의 active while loop는 제출 순서·poll(0)·
  park 조건이 그대로다. 바뀐 것은 `replyDrainer.drain(...)`의 반환값을 누적하는 것(long 덧셈 1회)뿐이고,
  `drainReplies` 본문의 분기·기록 경로는 그대로다.
- timeout·sleep·client 수·크기·HWM·상한을 바꾸지 않았다. teardown 창은 이전과 같은
  `max(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS, duration×3 s)`이고 `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS`
  기본값 5,000도 그대로다. runner의 server shutdown 창도 손대지 않았다.
- 인위적 in-flight cap 없음(D-BP15). teardown drain은 새 제출을 하지 않고, 상한 숫자를 도입하지 않으며,
  갚아야 할 echo 수는 Core가 실제로 admission한 건수 그대로다.
- 오류 삼키기 없음. teardown drain에서 받은 reply는 RESULT에 넣지 않을 뿐 버려지는 오류가 없고,
  send terminal 실패·창 초과는 종전과 같은 예외·문구로 올라간다.
- 지정 파일 밖을 건드리지 않았다. relay(`PerfMultiRoutedRelay*.java`)와 REQREP은 그대로다.

## 5. 검증

- 단위 테스트: `PerfMultiTargetCoordinatorTest`에 3건 추가(총 12건), `:perf-multi:test` 통과.
  - terminal이 안 끝나면 창 동안 계속 수신 turn을 돈다.
  - terminal이 끝나도 갚아야 할 echo가 남아 있으면 창을 닫지 않는다.
  - 창을 다 써도 terminal이 안 오면 정책 timeout 문구로 실패한다(블라인드 블록 금지).
- perf(모두 ticket 경유, 고정 Core prefix, tcp, 100 clients, duration 5):
  - **티켓 검증 — `--msg-sizes 4096 --runs 5`, 두 pattern 10/10 complete**
    ticket `2-1788842821-10317-claude-java-rrss`, rc=0,
    report `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260908_134719_javarrfix2.txt`
    (`success: 2`, `fail: 0`, `status: complete`, `expected_result_lines: 50 / actual: 50`).
    RR 4096 B median 135,669 ops/s(run별 149.3/138.8/135.7/129.5/133.2 K),
    DR 4096 B median 237,873 ops/s.
    teardown log:
    `CLIENT_DRAIN_DETAIL,exit,pending=0,drained=699318,outstanding=0,elapsed_ms=1502`(RR),
    `...,drained=517242,outstanding=0,elapsed_ms=2059`(DR).
    두 server log 모두 `RELAY_DRAIN_DETAIL`·`RELAY_FAILURE_DETAIL` 없음.
  - **회귀 — 64 B 3-run median**: 아래 §6.

## 6. 회귀(64 B)

기준: RR `javadrain2`(1-run) 421,550.4 ops/s, DR `r13java3`(3-run) 380,659.2 ops/s. 허용 -5%.

ticket `2-1788843086-45404-claude-java-rrss`, rc=0, 3-run median,
report `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260908_135251_javarrfix64b.txt`
(`fail: 0`, `status: complete`).

| pattern | 기준 | 지금(3-run median) | 차이 |
|---------|------|--------------------|------|
| RR 64 B | 421,550.4 | **429,465.4** (429.5/435.5/428.6 K) | **+1.9%** |
| DR 64 B | 380,659.2 | **365,347.0** (346.5/365.4/374.6 K) | **-4.0%** |

둘 다 -5% 안이다. 앞선 1-run 측정(`perf_java_multi_linux_20260908_135023_javarrfix64.txt`)에서는
DR이 337,470 ops/s(-11.4%)였으나, 같은 셀의 과거 값 분포가 337~412 K로 넓고(load 4~5 구간)
3-run median에서 -4.0%로 돌아왔다. 처리량은 `totalCount / durationSeconds`
(`PerfMetricsCollector.finish:118`)로 계산되므로 teardown drain 시간은 RESULT에 들어가지 않는다.
teardown 비용 실측: 64 B에서 RR 2 ms(`drained=458`), DR 754 ms(`drained=255093`).

## 7. 남은 것

- 4096 B의 절대 처리량(RR 130~150 K)은 이번 수정의 대상이 아니다. C 대비 격차는
  `2026-09-08-java-rr-sendsend-4096.ko.md` §5의 항목 그대로 남는다.
- relay의 drain 창(`PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 5,000 ms)과 runner의
  `SERVER_SHUTDOWN_TIMEOUT_MS`(5,000 ms)가 같은 값이라, relay가 자기 창을 끝까지 쓰면 JVM이 종료할
  시간이 남지 않아 `server_exit_124`가 된다. 이번 수정으로 relay가 창을 끝까지 쓰는 경로 자체가
  사라졌지만, 두 값이 같다는 구조는 그대로 남아 있다 — 정책 값이라 여기서는 손대지 않았다.
- client의 teardown drain은 이제 owed echo까지 기다리므로 run당 1.5~2 s가 더 걸린다.
  client 창((duration+20) s = 25 s) 안이고 RESULT 집계에는 들어가지 않는다.
