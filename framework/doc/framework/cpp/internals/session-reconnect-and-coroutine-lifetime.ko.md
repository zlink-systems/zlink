# 세션 재접속 수명주기와 코루틴 수명 규칙

이 문서는 C++ 런타임이 공통 서버 spec(03 상호작용 모델, 05 비동기 실행 정책,
26 message flow tracing)을 구현할 때 C++ 고유의 수명·동시성 제약 때문에 지키는
내부 불변식을 기록한다. 관찰 가능한 계약은 공통 spec이 소유하며, 이 문서는 그
계약을 C++의 코루틴·asio 기반에서 깨지지 않게 하는 구현 세부만 다룬다.

## 1. 재접속 binding 정체성

같은 클라이언트가 재접속하면 ROUTER가 stream rid를 재할당할 수 있고, 세션-owner
가 같은 rid를 다시 받을 수도 있다. 이때 옛 바인딩과 새 바인딩을 구별하지 못하면
registry가 닫힌 스트림 참조를 유지해 "송신 ok, 수신 무" 블랙홀이 된다. 다음
불변식을 지킨다.

- **Binding generation은 앱 lifecycle 전역 단조다.** 발급원은 세션-owner
  lifecycle의 전역 `next_binding_token`이며, 이 값이 STREAM bind → registry →
  command 38 전달까지 실경로에 그대로 사용된다. 같은 rid가 재사용돼도 세대는
  반드시 증가한다(재접속 시 `bg 1→2`).
- **Registry는 물리 세션을 binding token으로 판정한다.** `(rid, bg)`가 같아
  보여도 token이 다르면 새 물리 세션으로 취급해 스트림 참조와 sink를 원자적으로
  교체한다.
- **Replacement handler는 rid별 복수 등록을 유지한다.** 같은 rid에 옛/새 바인딩
  의 handler가 공존할 수 있으므로, 옛 등록부터 exact fence를 만족한 첫 handler만
  실행해 옛 스트림만 close한다. 교체 완료 콜백은 동기 대기 없이 asio steady
  timer(100ms)로 close를 예약한다 — 세션 serial lane을 잡은 채 잠들면 무관한
  세션의 진행을 막는다(계약 게이트 CPP-SESS-004).
- **Disconnect는 owner 바인딩을 tombstone 처리한다**(38 교환). 전송은 sink에
  캡처된 값이 아니라 send 시점의 current route를 사용한다.

## 2. 세션 ingress·relay·push의 소유권

- ingress drain은 route-교체 span이 소유한다.
- one-way relay는 FIFO/backlog admission 시점에 terminal이다(spec 05의 mailbox
  경계). 수신 완료를 caller 성공 조건으로 되돌리지 않는다.
- push는 per-binding FIFO에 넣고 offload executor로 detach한다. `task_t`는
  eager라서 코루틴을 그대로 호출하면 ROUTER admission까지 동기로 진입해 actor
  턴을 블록한다 — actor 턴에서 push를 시작할 때는 반드시 detach 경로를 쓴다.
- **stream 동기 dispatch는 같은 세션 lane에서의 동기 재진입을 지원하지 않는다.**
  `stream_session_dispatcher_t::dispatch()`(`stream_runtime.cpp`)는 세션별
  serial lane(`serial_execution_queue_t`, `serial_lane_policy_t::session`)에
  작업을 `dispatch_async`로 넣은 뒤 `task.result ()`로 완료를 블로킹 대기한다.
  이미 그 lane 위에서 실행 중인 세션 콜백(connected/packet/disconnected/error
  핸들러)이 같은 stream에 대해 동기 `dispatch_serial`/`dispatch()`를 다시
  부르면, serial lane이 caller 자신에게 점유되어 있어 새 작업이 시작될 수 없고
  대기는 영원히 풀리지 않는다(자기-데드락). 세션 콜백 문맥에서 같은 lane에 추가
  작업을 넣을 때는 완료 콜백을 받는 `dispatch_serial_async`/`dispatch_async`
  경로만 사용한다. 동기 형태는 lane 밖(호스트/테스트 드라이버) 전용이다.

## 3. 코루틴 수명 규칙

- 코루틴 파라미터는 프레임에 복사된다. suspend를 넘는 인자는 참조가 아니라 값
  으로 전달한다(`seal_bound_sessions`, `run_spot_publish_fanout`에서 확립).
- `catch` 블록 안에서 `co_await`하지 않는다.
- 같은 인자 목록에서 한 인자를 읽고 다른 인자를 `std::move`하는 것은 평가 순서
  미정(unsequenced)이다. 호출 전에 지역 변수로 hoist한다.
- 멤버 코루틴에서 `co_await` 이후 `this`를 만지지 않는다. 소유 객체가 caller
  스택 임시일 수 있으므로 필요한 공유 상태는 코루틴 진입 시 프레임 로컬로
  복사한다(`channel_outbound_exchange_t::submit_send/submit_publish`에서 확립).
- **mutex를 보유한 채 task 완료(`complete`)를 호출하지 않는다.** `task_t`는
  eager라서 완료가 continuation을 그 자리에서 인라인 재개하고, 재개된 코루틴이
  같은 mutex를 다시 잡으면 한 스레드 안에서 자기-데드락이 된다(Bingo
  authenticate에서 gdb로 실증: bind 완료 람다가 manager mutex를 쥔 채 token
  task를 완료 → 재개된 핸들러의 `find()`가 같은 mutex 대기). 완료할 completion
  source를 지역으로 옮겨 락을 푼 뒤 호출하거나 offload executor로 detach한다.

## 4. Message flow tracing 게이트

- 각 처리 지점은 live level을 한 번 읽고 즉시 분기한다(spec 26 §4). entry에서
  만든 flow context의 level snapshot은 이후 런타임 변경을 덮지 않는다 —
  `message_flow` 단위 테스트가 "normal로 진입 후 off 전환 → 이후 지점 침묵"을
  고정한다.
- tracer는 lazy 빌더(`Fn build_event`)를 우선 사용한다. 게이트를 통과한 뒤에만
  event 객체·문자열이 만들어지므로 Off는 read+branch 비용만 낸다.
- dispatch error는 `dispatch_error_reporter_t` 단일 경로로 보고한다. 테스트는
  flow observer가 아니라 `set_dispatch_error_observer_for_tests` 훅으로 dispatch
  error 이벤트를 구조적으로 검증한다(flow 이벤트에는 error_reason이 실리는
  경로 — 예: deferred Actor Join 실패의 replied/failed — 도 있으므로 두 훅을
  용도에 맞게 구분한다).
