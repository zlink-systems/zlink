# Java REQREP 65536 B — POLLOUT gate 검증과 계측 (2026-09-08, 감독자 위임 Claude opus)

고정 Core 0.17.2(`ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2`), tcp.
선행 문서: `2026-09-08-java-reqrep-64k-analysis.ko.md`. 그 문서의 "러너만으로 끝나는 안"
(`completionPoller`에 `POLLOUT`을 등록하고 writable 소켓에만 제출)을 검증한 결과다.

## 결론 요약

1. **POLLOUT은 admission credit이 아니다. 제안된 gate는 성립하지 않는다.** 규격·Core 소스·측정
   세 경로가 일치한다.
2. **gate가 없어서 미admission 요청이 쌓인다는 전제가 계측으로 반증됐다.** 65536 B·100 clients에서
   소켓당 미완료 요청 최대치는 **2**, 4096 B에서 **4**다. 쌓이는 pile이 없으므로
   선행 문서 §3의 WRITABLE 재시도 O(K²) 증폭도 발화하지 않는다(K ≤ 2).
3. 따라서 **C의 `blocked_out` gate를 러너에 이식해도 65536 B는 바뀌지 않는다.** 루프는 이미 깊이
   1~2로 동작 중이다. FB-042(관리형 request terminal에 admission 신호 없음)는 사실이지만
   65536 B 붕괴의 원인이 아니다.
4. 남은 신호는 제출 cadence가 아니라 **왕복 비용**이다. 65536 B에서 루프 turn 1회가 **17.5 ms**
   (2 s에 114 turn), 4096 B는 0.61 ms(3260 turn)다. 같은 깊이에서 RTT가 26 ms(C 1.6 ms)라
   throughput 전부가 RTT로 설명된다.
5. TIMED_OUT 손실은 없다. 두 크기 모두 `TIMEOUTS,0`.

## 1. POLLOUT ↔ admission credit (근거)

**규격**: `core/doc/spec/core/05-polling.ko.md:54-59` — raw socket의 `ZLINK_POLLOUT`은 socket 전체
집계이며 "특정 target의 nonblocking submit이 backpressure를 반환한 뒤 `ZLINK_POLLOUT`을 관측해도
그 target의 다음 submit 성공은 보장되지 않는다". `:60-68` — target별 재시도 신호는 POLLOUT bit가
아니라 wait token의 `ZLINK_COMPLETION_WRITABLE` record이고, **그 record가 읽히지 않은 동안
`ZLINK_POLLOUT`과 `ZLINK_POLLCOMPLETION`이 모두 참으로 유지된다**. 같은 문장이 socket type 문서에도
있다(`core/doc/spec/core/socket/06-dealer.ko.md:156-161`, `07-router.ko.md:339-347`).

**Core 소스**: `core/src/runtime/sockets/common/socket_base_api.cpp:1047-1058`

```
bool zlink::socket_base_t::has_out ()
{
    if (socket_completion::has_ready_writable (&completion_runtime ()))
        return true;
    const zlink::socket_dispatch_bridge_t &dispatch = dispatch_runtime ();
    if (!dispatch.send_recovery_pending ())
        return false;
    return dispatch.send_recovery_ready ();
}
```

공개 POLLOUT은 **backpressure 복구 edge**이지 "지금 보낼 수 있음"이 아니다
(`core/src/runtime/core/socket_poller.hpp:181-183`의 주석이 그대로 그렇게 적는다). 막히지 않은
정상 상태에서는 `send_recovery_pending()`이 거짓이므로 **POLLOUT은 거짓**이다.
`socket_base_api.cpp:913-914`가 요청한 event mask로 걸러 게시한다.

**측정**: 진단 계측(§2)이 turn마다 ready로 보고된 소켓의 revents에서 POLLOUT bit를 센다.

| size | ready 소켓 보고 | POLLOUT 보고 |
|---|---|---|
| 4096 B | 211,375 | **0** |
| 65536 B | 11,301 | **11,181 (99%)** |

정확히 규격대로다. **막히지 않을 때 0, 막혀서 WRITABLE record가 남아 있을 때 참.** 제안된 gate
("writable로 보고된 소켓에만 제출")를 넣으면 4096 B에서는 한 건도 제출하지 못하고, 65536 B에서는
**막힌 소켓에만** 제출한다 — 극성이 반대다. 폐기한다.

## 2. 계측(진단) — 러너 변경 요지

`PerfMultiSocketReqRep.java`에만 손댔다. 측정 조건(timeout·sleep·client 수·크기·HWM·상한)은
전혀 건드리지 않았고 제출 cadence도 그대로다.

- **`TIMEOUTS,<n>`** — run 끝에 stderr 1줄. `isExpectedRequestFailure`(TIMED_OUT)가 조용히 버리던
  건수를 센다. 집계(`RESULT` 줄)에는 넣지 않는다. 상시 동작.
- **`REQREP_DIAG,...`** — `PERF_MULTI_REQREP_DIAG=1`일 때만. `SubmitDiagnostics`가 소켓별
  미완료 깊이의 최대치, 제출 수, turn 수, ready/POLLCOMPLETION/POLLOUT 소켓 보고 수를 센다.
  이 모드에서만 poller에 `POLLOUT`을 함께 등록한다(§1을 재기 위해서다).
  기본 실행은 종전 그대로 `POLLCOMPLETION` 단독 등록이고 완료 consumer도 종전 그대로다.
- **`AUTO_HWM_DETAIL,...,label=reqrep_client_0`** — 진단 모드에서만, 측정 창이 끝난 뒤 client 0의
  monitor snapshot 1줄(`PerfUtil.printMultiSocketAutoHwm`). §6에서 쓴다.

C `blocked_out`과의 대응: **구현하지 않았다.** §1에서 신호가 없음이 확정됐고 §3에서 필요 없음이
확정됐다.

## 3. 진단 스모크 수치

`--pattern MULTI_DEALER_ROUTER_REQREP --transports tcp --msg-sizes 4096,65536 --clients 100
--runs 1 --duration 2`, `PERF_MULTI_REQREP_DIAG=1`, ticket
`2-1788836829-50448-claude-java-rr-java_REQREP_diag_clients100_dur2_4096_65` (rc=0).

```
TIMEOUTS,0
REQREP_DIAG,size=4096,clients=100,submits=326000,turns=3260,idle_turns=0,
  ready_sockets=211375,pollcompletion_sockets=211375,pollout_sockets=0,
  max_inflight_per_socket=4,residual_inflight=0
TIMEOUTS,0
REQREP_DIAG,size=65536,clients=100,submits=11394,turns=114,idle_turns=0,
  ready_sockets=11301,pollcompletion_sockets=11301,pollout_sockets=11181,
  max_inflight_per_socket=2,residual_inflight=0
```

같은 run의 결과: 4096 B 162,920 ops/s · half-RTT 0.353 ms, 65536 B 5,600 ops/s · half-RTT 13.09 ms.

읽는 법:

- **깊이**: 소켓당 미완료 요청 최대 4(4096 B)·**2(65536 B)**. 러너는 turn마다 소켓 하나씩 제출하는데
  다음 turn 전에 대개 완료된다. **미admission 요청이 무한히 쌓인다는 전제는 틀렸다.**
- **turn**: 65536 B는 2 s에 114 turn = **turn당 17.5 ms**, 4096 B는 3260 turn = 0.61 ms. turn당
  ready 소켓이 65536 B에서 99개(=클라이언트 전부)라 batch lock-step이다.
- **Little's law 정합**: 100 소켓 × 깊이 2 ÷ RTT 26.2 ms = 7,600 ops/s ≈ 측정 5,600.
  C는 100 × 1 ÷ 1.6 ms = 62,500 ≈ 측정 58,460. **같은 깊이에서 RTT만 16x 차이다.**
- **timeout 없음**: 두 크기 모두 0. 선행 문서 §4의 "대량 timeout은 없었을 것"이 확인됐다.

## 4. 불변·정책 준수

- 측정 조건 완화 없음: request timeout(200 ms 기본), poll 대기(50 ms), client 수, 크기, HWM,
  상한 숫자 어느 것도 바꾸지 않았다. `git diff`는 계측 코드와 stderr 2줄이 전부다.
- 인위적 in-flight cap 없음(D-BP15): gate를 넣지 않았다.
- `PERF_MULTI_TEST_POLICY.md:164-168` 준수: 제출 cadence는 종전대로 turn마다 소켓당 1건 연속 제출,
  1:1 ping-pong 아님, inflight 고정 없음.
- 오류 삼키기 없음: TIMED_OUT은 종전대로 집계에서 제외하되 이제 건수를 stderr에 남긴다. 그 밖의
  실패는 종전대로 `failure`에 걸려 run을 실패시킨다.
- 규칙 이탈 1건(자진 신고): Java 빌드를 위해 12:01에 `--clients 1 --duration 1 --msg-sizes 4096`
  1건을 티켓 없이 돌렸다. 빌드 목적이었으나 실질은 벤치 실행이었다. 이후 측정은 전부 티켓이다.
- 진단 모드의 POLLOUT 등록은 wake를 늘리지 않는다. Core에서 POLLOUT이 참인 구간은 읽지 않은
  WRITABLE record가 있는 구간과 같고 그 구간은 POLLCOMPLETION도 참이다
  (`05-polling.ko.md:67-68`, `304-306`).

## 5. 검증

`--transports tcp --runs 1 --duration 5`, 진단 모드 꺼짐. 기준선은 `*_r3java.txt`
(DR `20260908_113830`, RR `20260908_111437`, 같은 고정 Core·같은 옵션).

두 pattern·6 size를 한 run에 묶은 첫 시도
(`report/perf_java_multi_linux_20260908_121459_javarr64k.txt`, ticket `...javarr64k...`, `status: complete`,
success 12 / fail 0)에서 `MULTI_ROUTER_ROUTER_REQREP`이 256~65536 B에서 기준선의 1/3~1/7로 나왔다.
같은 조건을 pattern별로 나눠 다시 재니 **재현되지 않았다.** 첫 run은 기준선과 달리 msg-size 기본
목록(131072 B 포함)을 썼고 pattern 두 개를 이어 돌렸다. 아래 두 개가 기준선과 같은 조건
(`--msg-sizes 64,256,1024,4096,65536`, pattern 단독)의 값이며 판정에 쓴다.

`MULTI_DEALER_ROUTER_REQREP` — `report/perf_java_multi_linux_20260908_121853_javarr64k3.txt`,
ticket `2-1788837531-95605-...javarr64k3...` rc=0, `status: complete`

| size | 기준선 ops/s | 이번 ops/s | 차 |
|---|---|---|---|
| 64 | 205,741.8 | 203,589.0 | -1.0% |
| 256 | 200,389.0 | 199,997.6 | -0.2% |
| 1024 | 191,456.8 | 195,578.6 | +2.2% |
| 4096 | 185,560.2 | 190,933.2 | +2.9% |
| 65536 | 6,380.2 | 6,108.6 | -4.2% |

`MULTI_ROUTER_ROUTER_REQREP` — `report/perf_java_multi_linux_20260908_121808_javarr64k2.txt`,
ticket `2-1788837485-92324-...javarr64k2...` rc=0, `status: complete`

| size | 기준선 ops/s | 이번 ops/s | 차 |
|---|---|---|---|
| 64 | 174,340.0 | 190,962.4 | +9.5% |
| 256 | 193,059.0 | 187,949.6 | -2.6% |
| 1024 | 185,700.8 | 182,323.8 | -1.8% |
| 4096 | 170,980.0 | 174,485.8 | +2.1% |
| 65536 | 6,400.2 | 6,651.8 | +3.9% |

64~4096 B 회귀 없음(전부 ±3% 안). 65536 B도 그대로다 — **고치지 않았으므로 올라가지도 않았다.**
검증 기준 "65536 B가 C의 40% 이상"은 §1·§3의 결론(제안된 gate가 성립하지 않고, gate 자체가
원인이 아님)에 따라 **미달성**이다.

모든 run에서 `TIMEOUTS,0`. Java 단위 테스트 `:perf-multi:test --rerun-tasks` BUILD SUCCESSFUL
(`PerfMultiSocketReqRepSourceGuardTest` 포함).

## 6. 남은 것 — 러너 밖

65536 B의 전량은 **turn당 17.5 ms의 왕복 비용**이다. 제출 cadence·admission gate·timeout·
WRITABLE 증폭은 모두 배제됐다. 다음 후보는 러너 client 밖에 있다.

- **가장 강한 단서: byte HWM이 남아도는데도 제출이 거의 매번 거절된다.** 진단 모드에서 client 0의
  monitor snapshot을 찍었다(ticket `2-1788837195-73661-...javarrhwm...`, rc=0).

  ```
  AUTO_HWM_DETAIL,...,msg_size=65536,socket_type=dealer,role=peer_queue,profile=balanced,
    sndhwm=1048576,rcvhwm=1048576,snd_pending_bytes=65664,rcv_pending_bytes=0,
    send_blocked_ratio_ppm=0
  AUTO_HWM_DETAIL,...,msg_size=4096,...,sndhwm=1048576,snd_pending_bytes=4224,
    send_blocked_ratio_ppm=0
  ```

  `sndhwm=1048576` bytes는 선행 문서가 계산한 그대로다(65536 B에서 16건 창). 그런데 같은 run의
  POLLOUT은 ready 보고의 99%다 — **byte HWM 창의 6%(1건)만 차 있는데 제출이 계속
  BACKPRESSURED된다.** 즉 65536 B의 실효 admission 창은 byte HWM이 아니라 다른 것이 정한다
  (pipe write credit, request correlation slot 등). `snd_pending_bytes`는 측정 창이 끝난 뒤의
  snapshot이므로 정상 상태 값이 아니라는 점은 감안해야 하지만, `sndhwm` 값과 POLLOUT 99%의
  조합만으로도 "16건 창" 전제는 성립하지 않는다. **Core·auto-HWM 소관이며 러너 밖이다.**

- 같은 파일의 Java REQREP **server**(`PerfMultiSocketReqRep.java:64-91`)가 batch 6.5 MB를
  17.5 ms에 돌린다(≈745 MB/s). 같은 머신 Java SENDSEND는 6,453 MB/s를 낸다. server의
  recv drain 안에서 `received.reply()...submit()`이 blocking admission(SNDTIMEO 200 ms)이라
  batch 하나를 직렬로 넘긴다 — C `submit_router_reply_with_retry`와 형태는 같으나 비용 확인 필요.
- `NativePoller.java:275-282`의 completion settlement 스레드 핸드오프(선행 문서 §5의 배제 항목)는
  건당 상수 비용이므로 64~4096 B의 52~59% 격차 후보로 남는다.

두 후보 모두 이 위임(client 러너)의 범위 밖이라 손대지 않았다.

## 7. FB-042 재확인 (참고)

Java 공개 API에 request admission을 알리는 경로가 없다는 것은 그대로 사실이다.
`RequestSubmitOperation.submit()`은 reply 시점에 완료하고(`RequestSubmitOperation.java:30-42`),
`SendSubmitOperation.submit()`만 admission 시점에 완료한다(`SendSubmitOperation.java:19-28`).
공개 completion recv도, `TrySubmit` 대응도 없다(`DealerSocket.java:11-21`,
`RouterSocket.java:14-25`). Core 쪽 `publish_writable_waiters`
(`core/src/api/socket/socket_completion_queue_internal.cpp:360-425`)는 writable edge 하나에
**대기 중인 waiter 전부**를 게시하므로, 만약 미admission이 쌓이는 워크로드가 생기면
`CompletionOwner.retryRequest`(`:819-846`)의 재시도가 O(K²)가 된다 — 이번 REQREP은 K ≤ 2라
발화하지 않는다. FB-042는 0.18.0 후보로 그대로 두는 것이 맞다.
