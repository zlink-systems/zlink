# Go Multi ROUTER↔ROUTER SENDSEND 65536 B — relay가 STOP 뒤 종료하지 못한 이유

## 결론

Go relay server가 STOP 뒤 5초 안에 끝나지 못한 원인은 teardown 자체가 아니라
**echo loop이 이미 admit된 reply 하나의 blocking `Submit`에서 영원히 깨어나지
못하는 것**이었다. client가 RESULT를 찍고 퇴장한 뒤에는 그 route의 WRITABLE
token을 완료시켜 줄 주체가 없고, token을 풀어 줄 socket close는 main goroutine이
`<-serverDone`에서 그 echo loop을 기다리느라 실행되지 못한다. 서로가 서로를
기다리는 교착이다.

C relay(`bindings/c/perf/multi/common/perf_multi_relay_server.hpp:524-600`)는
STOP 뒤 wait token을 `send_retry_drain_timeout_ms()` 안에서만 재시도하고 그 뒤에는
정상 teardown으로 넘어간다. Go에는 그 bound가 없었다. 같은 bound를 Go에 넣어
고쳤다.

## 측정 범위와 기준

- source commit: `5c62a8d2c3`
- Core source: `release`
- Core package prefix: `/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha`
- pattern `MULTI_ROUTER_ROUTER_SENDSEND`, transport `tcp`, 65536 B,
  client 100, duration 5초
- 수정 파일: `bindings/go/perf/multi/perf_multi_router_router.go` 하나

## 1. 실패 단계와 근거

`PERF_GO_SHUTDOWN_STACK_DUMP=1`로 STOP 뒤 0.5초마다 stage와 goroutine stack을
server stderr에 찍었다(티켓 로그
`.artifacts/perf-queue/log/2-1788842431-68884-claude-go-rrss-go_RR_sendsend_65536_shutdown_diag.log`).
5-run 중 2회가 같은 모습으로 실패했다.

```
[perf-multi-relay] shutdown stalled stage=echo_loop elapsed_ms=500  reply_submit_started=202880 reply_submit_finished=202879 reply_submit_inflight=1
[perf-multi-relay] shutdown stalled stage=echo_loop elapsed_ms=4000 reply_submit_started=202880 reply_submit_finished=202879 reply_submit_inflight=1
```

STOP 뒤 4초가 지나도록 stage는 `echo_loop`에 머물고, 202,880번째 reply submit
하나만 끝나지 않았다. socket close(`close_socket`)에는 들어가 보지도 못했다.
goroutine dump가 그 한 건의 위치를 정확히 짚는다.

```
goroutine 10 [chan receive]:
zlink.systems/zlink/internal/native.(*completionEntry).waitSend(0xc0000c4240)
	bindings/go/internal/native/completion_owner.go:299
zlink.systems/zlink/internal/native.submitManagedSend(...)
	bindings/go/internal/native/dealer_router_request.go:284
...
main.submitMultiRouterReply(_, {{0x72, 0x6f, 0x75, 0x74, 0x65, 0x72, 0x2d, 0x30, 0x30, ...}, ...}, ...)
main.startMultiRouterRouterEchoServer(...)

goroutine 1 [chan receive]:
main.runMultiRouterRouterServer(...)   # <-serverDone
```

- goroutine 10(echo loop)은 `router-0…` route로 가는 reply의 WRITABLE 완료를
  `waitSend`에서 기다린다. 그 client 프로세스는 이미 RESULT를 찍고 종료했다
  (러너는 client 종료를 확인한 뒤에야 `shutdown_server`로 STOP을 보낸다,
  `bindings/go/perf/run_benchmarks_multi.sh:1382-1393`).
- goroutine 1(main)은 `<-serverDone`에서 goroutine 10을 기다린다. 그래서
  `server.Close()`가 실행되지 않고, `Close()`가 해 줄 pending entry 해제
  (`completion_owner.go:928 shutdownOwner` → `entry.shutdown()`)도 일어나지 않는다.
- goroutine 18(binding의 runtime completion drain)은 살아서 poll하고 있었다.
  drain이 멈춘 것이 아니라, 사라진 peer의 wait token에 대한 완료가 오지 않는다.

64~4096 B에서 나지 않는 이유는 크기가 작을수록 마지막 reply가 admit 즉시
끝나 token이 남을 확률이 낮기 때문이고, `MULTI_DEALER_ROUTER_SENDSEND`가
같은 echo 함수로도 통과한 것은 같은 이유의 확률 차이다(RR client는 route가
100개 ROUTER socket으로 흩어져 마지막 순간 backpressure가 남기 쉽다).

## 2. 수정과 C 대응

`bindings/go/perf/multi/perf_multi_router_router.go`:

1. **STOP 뒤 bounded drain.** echo loop이 reply submit에 쓰는 context를
   하나 만들고, control STOP이 오면 `relayShutdownDrainWindow()` 뒤에 그
   context를 cancel한다. 이미 admit된 reply는 그 창 안에서 완료될 기회를
   그대로 갖고, 창이 지나면 `Submit`이 `context canceled`로 돌아와 loop이
   빠져나오며 정상 teardown(socket → context close)이 실행된다.
   C의 `perf_multi_relay_server.hpp:524-600`(STOP 뒤 drain_deadline 안에서
   `drain_reply_writable` 재시도 → 그 뒤 정상 teardown)과 같은 구조다.
2. **창 길이.** `min(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS,
   PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS − 2 s)`, 하한 250 ms. 기본값에서는
   3,000 ms이고 남은 2,000 ms가 close 몫이다. C는 drain을
   `send_retry_drain_timeout_ms()`(5,000 ms)로 잡지만 C의 drain loop은 poll
   기반이라 close 시간이 따로 필요 없다. Go는 blocking terminal을 깨우는
   비용을 러너 예산(`SERVER_SHUTDOWN_TIMEOUT_SECONDS`) 안에 넣어야 해서
   teardown 몫을 남긴다.
3. **stage trace.** STOP 이후에만 도는 watchdog이 0.5초마다 stage·in-flight
   reply 수를 stderr에 찍는다. 정상 종료(<500 ms)에서는 아무것도 찍지 않는다.
   `PERF_GO_SHUTDOWN_STACK_DUMP=1`이면 첫 tick에 goroutine dump를 덧붙인다.
   러너가 SIGTERM/SIGKILL로 죽인 프로세스는 사후 출력이 없으므로, 예산이
   흐르는 동안 stderr로 나와야만 근거가 남는다.
4. `submitMultiRouterReply`가 context를 받도록 바꾸고
   `SubmitMeasurementSendContext`를 쓴다. `IsStaleRoute` 처리는 그대로다.

같은 echo 함수를 쓰는 `MULTI_DEALER_ROUTER_SENDSEND` server도 같은 bound를
얻는다.

## 3. 불변 준수

- **측정 창은 그대로다.** context cancel은 control STOP이 온 *뒤에만* arm된다.
  active window 동안 reply admission은 여전히 무제한 blocking terminal이고
  deadline이 없다. 인위적 in-flight cap을 넣지 않았다(D-BP15).
- **timeout·client 수·크기·HWM 상한 완화 없음.** 새로 추가한 시간은 러너의
  shutdown 예산 안쪽을 쪼갠 것뿐이고, 러너 인자·socket option은 건드리지 않았다.
- **오류를 삼키지 않는다.** drain 창이 만료되어 포기한 reply는
  `[perf-multi-relay] reply abandoned after shutdown drain: <err>` 한 줄로
  server stderr에 남는다. 그 외 send 오류는 전과 같이 `Must`로 즉시 실패한다.
- 정책·스펙·계획서와 Core·`framework/**`·다른 언어는 건드리지 않았다.
  Go 쪽도 `perf_multi_router_router.go` 한 파일만 바꿨다.

## 4. 검증

`go vet ./perf/multi`, `go test ./perf/...` 통과. 아래는 모두 perf 티켓으로 냈다.

| 항목 | 결과 | report |
|---|---|---|
| RR SENDSEND tcp 65536 B 5-run (수정 뒤, `--reuse-build` 없이 재빌드) | 5/5 `complete`, fail 0 | `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260908_134513_gorrfix.txt` |
| 회귀: DEALER_ROUTER SENDSEND tcp 65536 B 1-run | `complete` | `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260908_134902_gorrfix-reg-dr.txt` |
| 회귀: RR SENDSEND tcp 64 B 1-run | `complete` | `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260908_134908_gorrfix-reg-64.txt` |
| 수정 전 진단 5-run | fail 2 (`server_shutdown_failed`) | `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260908_134118_gorrdiag.txt` |

수정 뒤에도 교착 자체는 여전히 발생하고(Core/binding 쪽 문제이므로),
bound가 그것을 정해진 시간 안에 끊는다는 것을 직접 재현으로 확인했다.
STOP → 종료까지 걸린 시간과 trace(티켓 로그
`.artifacts/perf-queue/log/2-1788842857-16249-claude-go-rrss-go_RR_sendsend_regression_plus_STOP_tear.log`):

```
iter 1: client_rc=0 server_rc=0 teardown_ms=150     (no relay trace)
iter 2: client_rc=0 server_rc=0 teardown_ms=150     (no relay trace)
iter 3: client_rc=0 server_rc=0 teardown_ms=100     (no relay trace)
iter 4: client_rc=0 server_rc=0 teardown_ms=2950
  [perf-multi-relay] shutdown stalled stage=echo_loop elapsed_ms=500..3000 reply_submit_inflight=1
  [perf-multi-relay] shutdown drain expired window_ms=3000 reply_submit_inflight=1
  [perf-multi-relay] reply abandoned after shutdown drain: multi router/router server send: context canceled
iter 5: client_rc=0 server_rc=0 teardown_ms=2950    (iter 4와 동일한 trace)
```

5회 중 2회에서 교착이 재현되었고, 두 번 모두 3,000 ms 창이 끊은 뒤 2,950 ms에
정상 종료(`server_rc=0`)했다. 러너 예산 5,000 ms 안이다.

## 5. 남는 문제 — Core/binding 후보

이 수정은 perf harness의 teardown을 러너 예산 안에 넣은 것이고, 원인 자체는
아래에 있다. 별도 판단이 필요하다.

- ROUTER `SendTo`가 backpressure로 WRITABLE token을 받은 뒤 **그 peer가
  연결을 끊으면 token이 완료되지 않는다.** blocking `Submit`은 context cancel
  이나 socket close 없이는 영원히 깨어나지 못한다.
- 대조적으로, peer가 사라진 뒤 *새로* 내는 submit은
  `SubmitNotConnected`/`SubmitNotFound`로 즉시 실패한다
  (`perfcommon.IsStaleRoute`가 그 경로를 다룬다). 즉 "admit 전"에는 route 소멸을
  보고하고 "admit 후 대기 중"에는 보고하지 않는 비대칭이다.
- C harness가 같은 증상을 덜 보이는 것은 C가 blocking terminal을 쓰지 않고
  wait token을 poll로 직접 관리하기 때문이다. 언어 binding의 blocking
  terminal은 이 비대칭에 그대로 노출된다.
