# Java REQREP 65536 B 붕괴 — 정적 분석 (2026-09-08, 감독자 위임 Claude opus, 읽기 전용)

고정 Core 0.17.2, tcp, 100 clients, 1-run. Java `MULTI_DEALER_ROUTER_REQREP`·`MULTI_ROUTER_ROUTER_REQREP`가
65536 B에서만 C 대비 10.9~11.2%(6,380 ops/s vs 58,460; half-RTT latency 11.7 ms vs 0.8 ms). 64~4096 B는 52~59%.

## 결론 요약

1. **"65536 B에서만"의 정체는 코드의 크기 분기가 아니라 byte HWM이다.** auto-HWM(balanced)의 `SNDHWM 1048576`은
   메시지 수가 아니라 bytes(C `perf_multi_runtime.hpp:326` `auto_hwm_applied_sndhwm_bytes`, Java
   `PerfAutoHwm.java:32-33`). 소켓당 admission 깊이: 64 B 16,384개 · 4096 B 256개 · **65536 B 16개**.
   Java binding·러너 전체에 64 KB 임계값은 없다(`LibraryLoader.java:183`의 파일 복사 버퍼만, 무관). JNI는 없다(FFM 전용).
2. **주원인(확신 높음): REQREP client 루프에 admission backpressure gate가 없다.**
   `PerfMultiSocketReqRep.java:216-233`은 매 턴 100 client 전부에 새 request를 제출한다. C reference
   `perf_multi_socket_reqrep.hpp:234-238`은 막힌 slot(`retrying && (!retry_ready || wait_token != 0)`)에는
   새 request를 만들지 않고 `blocked_out`으로 돌아간다(`:322` `retained_request = true`). Java binding
   `CompletionOwner.java:174-204`는 writable-wait이면 parts를 retain하고 평범한 future를 돌려주므로 러너가
   막힘을 볼 수 없다. `sent_ts_ns`는 제출 생성 시점(`:221-222`)이라 retained 대기가 latency에 그대로 들어간다.
   - 같은 저장소의 SENDSEND client는 `PerfMultiRoutedSendCoordinator.java:198-200` CAS gate(소켓당 미admission 1)로
     C 모델을 따르고 65536 B에서 정상(DD 53.7%, DR SS 49 K, RR SS 57 K ops/s). gate 없는 REQREP만 무너진다.
   - Little's law: Java 65536 B inflight ≈ 149(RTT 23.3 ms), C ≈ 94(RTT 1.6 ms) — 동시성은 같고 왕복 지연만 14x.
     대역폭 836 MB/s는 같은 머신 Java SENDSEND 6,453 MB/s의 1/7.7 → 지연은 큐 대기에서 온다.
3. **2차 증폭 후보(미검증): WRITABLE 재시도.** `CompletionOwner.java:82,86`의 `pending`·`retries`는 상한이 없고,
   `:628-643` `drainLocked`는 NO_DATA마다 retries 전부를 재시도, `:819-845`는 backpressure면 재무장한다.
   재시도 1회 = `zlink_msg_copy`×2 + `zlink_msg_close`×2(`:348-352`). Core가 writable 전이에 무장된 토큰 전부에
   WRITABLE을 내면 O(K²). Core 0.17.2 소스 미확인이라 차수는 모른다.
4. **진단 공백:** `isExpectedRequestFailure`(`:308-311`)가 TIMED_OUT을 카운터 없이 버린다. C는 drain timeout 시
   waiting_slots/replies를 stderr에 찍는다(`perf_multi_socket_reqrep.hpp:621-626`). 이번 run은 p99 18 ms(timeout
   200 ms의 1/10)라 대량 timeout은 없었을 가능성이 높다.
5. **배제:** 제출당 64 KB deep copy(`copyForSubmit` → `Message.from`)는 C도 동일(`perf_multi_socket_reqrep.hpp:255-259`);
   recv 경로 복사 없음(PUBSUB 65536 B 138%와 일치); server reply 경로는 더 많은 일을 하는 SENDSEND relay가 49 K를 내므로
   병목 아님; completion settlement의 스레드 핸드오프(`NativePoller.java:275-282` → `CompletionDispatcher`)는 크기 무관
   상수 비용(64~4096 B 52~59% 격차의 후보)이지 65536 B 붕괴 원인이 아니다.

## 최소 확인 실험(제안)

- ①(a) `CompletionOwner.submitRequest`의 writable-wait 분기 카운터를 러너 쪽에서 세어(소켓별 "writable 아님" 턴 수)
  stderr 1줄 — clients 8, duration 2, 4096/65536 B 스모크로 65536 B에서만 폭증하면 확정.
- ①(b) 진단용으로만 소켓별 1-pending gate를 넣고 65536 B 스모크 — 정책(1:1 ping-pong 금지) 위반이라 최종안은 아님.
- ①(c) `completionPoller`에 `POLLOUT`을 함께 등록해 턴당 writable 소켓 수를 로그.
- ② `drainLocked` NO_DATA 분기에서 `retries.size()` max/합 로그. ③ TIMED_OUT 카운터 `TIMEOUTS,<n>` 출력.

## 러너 수정으로 끝나는가

- gate 자체는 러너 문제이나, **Java request terminal에는 admission을 알리는 별도 신호가 없다**(stage는 reply 시점 완료).
  send terminal의 stage는 admission 시점 완료라 SENDSEND CAS gate가 성립한다. request에 같은 CAS를 쓰면 1:1
  ping-pong이 되어 정책 위반.
- **러너만으로 끝나는 안:** `completionPoller`에 `POLLOUT`을 등록하고 writable로 보고된 소켓에만 새 request 제출
  (C `blocked` 의미). 단 DEALER/ROUTER의 POLLOUT이 request admission credit을 반영하는지 검증 필요(미검증).
- **binding 변경이 필요한 안:** request terminal이 retained(미admission) 여부를 러너에 알리는 public 경로(admission-only
  stage 또는 소켓별 미admission 카운트) — spec 소관.
- ②는 binding 쪽(①이 고쳐지면 K가 자라지 않아 실질 무해 가능), ③은 러너 계측, ⑤(핸드오프)는 별도 항목.

후속: 러너 안(POLLOUT gate)을 Claude sub-agent에 위임(2026-09-08 11:58). 결과는
`2026-09-08-java-reqrep-64k-gate.ko.md`.
