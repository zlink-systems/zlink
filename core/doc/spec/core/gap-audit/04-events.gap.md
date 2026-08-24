# Events 스펙-구현 gap 감사

> 감사 도구: codex (정적 대조) · 2026-08-24
> 실행 테스트: 수행하지 않음 (감사 지침)

판정: **구현/문서 gap 5건, 요확인 0건**. receive-flow 런타임 동작과 공개 상수는 대상 스펙과 일치한다. 발견 건은 모두 §6이 요구한 공개 contract test의 미검증 항목이다. 코드와 대상 스펙은 수정하지 않았으며, 지정된 감사 보고서만 작성했다.

## 대조 완료 계약군

- monitor event·readiness·timer fire의 family 구분과 readiness의 level-triggered 성격: 일치
- raw socket lifecycle monitor가 application payload를 event record에 싣지 않는 경로와 monitor event record의 공개 field: 일치. monitor open/recv·queue overflow·status counter는 `06-monitoring.ko.md`가 소유하므로 중복 계상하지 않았다.
- DEALER/ROUTER 전용 receive-flow frame의 completion-lane 수신, stale/duplicate 거부, application pipe 적용 뒤의 PAUSED/RESUMED 발생: 일치
- PAUSED/RESUMED의 epoch·routing ID·pair ID·generation, RESUMED의 실제 writable flag, STALE의 generation/epoch flag와 `value` 선택: 일치
- receive-flow event mask bit `16`/`17`/`18`과 `ZLINK_EVENT_ALL == 0x7FFFF`: 일치
- monitor worker queue의 FIFO enqueue와 서로 다른 connection I/O thread에 대한 전역 wall-clock order 부재: 구현상 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| B. 구현 gap | `04-events.ko.md:97-99` — peer가 유지 중인 상태를 다시 요청하거나 상태를 바꾸지 않는 flow-state frame에서는 receive-flow event가 관찰되지 않아야 함 | `core/src/runtime/sockets/common/socket_base_flow_state.cpp:244-255`는 같은 state의 더 새 epoch를 record에만 저장하고 event를 내지 않는다. 그러나 `core/tests/integration/test_flow_state_c_api.cpp:206-231`의 idempotency 표본은 연결·monitor 없이 local setter 반환값만 확인하고, `:405-445`는 같은 epoch 재전송이 STALE event가 됨만 확인한다. | 공개 monitor로 같은 state의 **전진한 epoch**와 local repeated state가 모두 무발생임을 확인하는 contract test가 없다. duplicate stale 표본은 이 no-op 규칙의 대체 증거가 아니다. |
| B. 구현 gap | `04-events.ko.md:101-105` — PAUSED·RESUMED 모두 epoch와 peer `routing_id`/pair ID/generation을 담고, EPOCH STALE의 현재 epoch는 직전 PAUSED·RESUMED가 보고한 값과 같아야 함 | `core/src/runtime/sockets/common/socket_base_flow_state.cpp:332-349,377-401`은 두 transition에 같은 routing ID와 epoch를 기록한다. `core/tests/integration/test_flow_state_c_api.cpp:369-399`는 PAUSED의 routing ID와 두 event의 pair field만 확인하며, RESUMED의 routing ID는 확인하지 않는다. `:405-442`는 EPOCH STALE `value`만 확인한다. | 구현은 field 계약을 만족하지만, §6이 요구한 공개 표면 test가 RESUMED routing ID 및 STALE가 참조하는 직전 transition epoch의 대응을 검증하지 않는다. |
| B. 구현 gap | `04-events.ko.md:72-74,103` — byte HWM·transport wait·termination이 계속 막으면 RESUMED에는 `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE`이 없고, RESUMED만으로 다음 send 수락을 보장하지 않음 | `core/src/runtime/core/pipe.cpp:1376-1407`은 remote pause 외의 원인이 남으면 actual writable을 false로 계산하고, `core/src/runtime/sockets/common/socket_base_flow_state.cpp:340-349`는 그때 flag를 비운다. `core/tests/integration/test_flow_state_c_api.cpp:390-399`는 writable인 정상 RESUMED만 확인한다. HWM과 remote pause의 독립성은 `core/tests/integration/test_flow_state_paired.cpp:302-349`에서 internal send 관찰로만 확인한다. | 다른 차단 원인이 남은 RESUMED를 public monitor로 받아 flag가 비어 있고 다음 send가 아직 거부될 수 있음을 검증하는 contract test가 없다. |
| B. 구현 gap | `04-events.ko.md:107-109` — 세 event의 bit가 16·17·18이고 `ZLINK_EVENT_ALL`이 `0x7FFFF`이며, 직접 지정한 monitor는 선택 bit가 있을 때만 event를 받음 | `core/include/zlink_enum.h:241-269`이 값과 alias를 선언하고, `core/src/runtime/sockets/common/socket_base_monitor.cpp:612-635`이 mask membership일 때만 enqueue한다. `core/tests/integration/test_flow_state_c_api.cpp:199-200,351-504`는 세 bit를 모두 OR한 positive mask만 사용한다. | 숫자값/`ALL`을 직접 단언하거나, 각 bit를 제외한 mask에서 해당 event가 관찰되지 않음을 확인하는 public contract test가 없다. |
| B. 구현 gap | `04-events.ko.md:111-113` — 같은 monitor queue는 Core commit 순서를 보존하고, 서로 다른 connection I/O thread 사이에는 전역 wall-clock order가 없음 | `core/src/runtime/sockets/common/socket_monitor_runtime.cpp:243-264`은 worker queue에 FIFO로 추가하고, `core/src/runtime/sockets/common/socket_base_monitor.cpp:643-663`은 front부터 dispatch한다. `core/tests/integration/test_flow_state_c_api.cpp:351-403`은 한 DEALER/ROUTER pair의 PAUSED→RESUMED 순서만 확인한다. | 같은 monitor를 공유하는 복수 connection의 commit 순서 보존을 public recv/handler로 확인하는 표본이 없으며, 그 표본에서 I/O thread 간 절대 wall-clock 순서를 기대하지 않는 경계도 검증되지 않는다. |

## 요확인

- 없음.
