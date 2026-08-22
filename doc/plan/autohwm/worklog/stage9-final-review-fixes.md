# Stage 9: 최종 검토 지적 수정

최종 review gate가 남긴 core MEDIUM 2건과 plan 적합성 LOW 2건을 수정했다. 항목마다 재현
test를 먼저 작성해 red를 확인하고, 수정 뒤 green을 확인했다. 모든 실행은 `core/build-tests`다.

| # | 지적 | Test | Red 증거 | Commit |
|---|---|---|---|---|
| M1 | Monitor event payload 유실 | `test_pause_and_resume_each_emit_exactly_one_event`, `test_duplicate_frame_emits_stale_event`, `test_stale_generation_event_reports_the_received_generation` | flags가 0이고 STALE `value`가 엉뚱한 값 | `7fdc62811c` |
| M2 | Lifecycle 기록 누락 (pre-attach PAUSE, 종료 시 gauge) | `test_pause_applied_by_pair_admission_is_booked`, `test_paused_pair_lifecycle_keeps_gauge_and_events_matched` | `pause_applied 0`, gauge가 0으로 돌아오지 않음 | `4939cfa7ad`, `01a97c9781` |
| L1 | §8.1 byte charge·LWM test 누락 | `unittest_pipe_byte_charge` (7 tests) | 해당 계약을 검증하는 test 자체가 없었음 | `b37353a32f` |
| L2 | Registry dual-ledger 잔재 | 기존 sweep 유지 | — (삭제 전용) | `5983236d29` |

## M1. 어떤 모양을 골랐고 왜인가

`dispatch_monitor_event`는 내부 record의 `values[0]`만 공개 event로 옮긴다. 그래서 §6이
요구하는 RESUMED의 `actual_writable`과 STALE의 generation·epoch 문맥이 만들어지자마자
버려지고 있었다.

**고른 모양: 기존 공개 event의 `flags` 필드. ABI 변경 없음.**

`zlink_socket_monitor_recv (void *monitor_, zlink_socket_monitor_event_t *out_, ...)`는
**호출자가 할당한** struct에 쓴다. 필드를 뒤에 덧붙이면 예전 크기로 할당한 7개 FFI binding의
buffer를 넘어 쓴다. 확장이 아니라 hard break다. 반면 `flags`는 이미
"Event-specific flags"로 문서화된 자리이고 `ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE`가
같은 방식으로 쓰고 있다. 새 bit 세 개를 추가했다.

- `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` — RESUMED가 pipe를 실제로 writable로
  만들었는가
- `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION` — 다른 generation을 지칭한 frame
- `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH` — epoch가 전진하지 않은 frame

PAUSED/RESUMED는 `value`가 epoch 그대로이고 routing ID·pair ID·generation은 이미 같은
event의 별도 필드이므로 §6 목록이 그대로 채워진다.

STALE은 `value` 하나뿐인데 §6은 네 값을 말한다. 두 사유마다 **재구성할 수 없는 값이 정확히
하나**라서 `value`가 그 하나를 싣는다.

- generation stale: `value` = 받은 generation. 그 frame의 epoch는 죽은 connection의 것이라
  의미가 없고, 현재 generation은 event의 `transport_pair_generation`이다.
- epoch stale: `value` = 받은 epoch. 받은 generation은 정의상 현재 generation이고, 현재
  epoch는 같은 pair의 직전 PAUSED/RESUMED가 보고한 값이다.

사유 flag가 어느 쪽인지 말하므로 `value`의 의미는 모호하지 않다. 문서화된 필드 의미를 바꾸지
않았고 `transport_pair_generation`은 계속 현재 generation이다.

Test는 id와 개수만이 아니라 payload를 단언한다. Monitor probe가 event 전체를 기록하도록
확장했다(`test_monitor_probe_record_at`).

## M2. Lifecycle 기록

**(a) pair admission이 적용한 PAUSE.** 수락된 상태가 pipe에 적용되기 전에 record에 먼저
들어가면 `attach_pipe`의 재적용이 PAUSED↔RUNNING 전이를 수행한다. 이 경로가
`flow_state_applied ()`를 우회해서 PAUSED event도, gauge·total 증가도, pause 시작 시각도
없었고, 뒤따르는 RUNNING만 짝 없이 올라왔다. 이제 같은 기록 경로를 탄다. Event 발행은 옆의
edge들과 마찬가지로 table mutex 밖에서 한다.

**(b) paused 상태로 끊긴 pair.** RESUMED에 도달하지 못하므로 gauge의 +1을 짝지을 것이
없었고, disconnect-while-paused를 반복하면 gauge가 영구히 올라갔다. 수락된 상태를 소유한
lane의 종료가 slot을 반납하고 pause 측정을 닫는다. 이는 resume이 아니라 lifecycle 반납이라
RESUMED event를 내지 않고 `resume_applied_total`도 건드리지 않는다. Peer가 resume한 적이
없기 때문이다.

(a)의 결정적 재현: record만 채우고 socket을 drain하지 않아 frame command가 아직 돌지 않은
상태에서, inproc 검증 진입점으로 같은 attach 경로를 직접 부른다. (b)는 pause/disconnect를 네
번 돌려 gauge가 0으로 돌아오고 pause마다 PAUSED event가 정확히 하나인지 본다.

Cycle test의 handshake는 처음에 blocking recv로 썼다가 5회 중 1회 무한 대기했다. 앞 cycle이
아직 정리 중인 connection으로 "hello"가 사라질 수 있어서, 새 route가 하나를 실어 나를 때까지
재시도하도록 고쳤다. 이후 27회 연속 통과.

## L1. §8.1 byte charge·LWM test

계약은 §3.1·§3.2가 정확히 고정하는데 그 산술 자체를 단언하는 test가 없었다. 둘 다 순수
함수라 `unittest_pipe_byte_charge`가 test 전용 창을 통해 직접 단언한다.

- 일반 frame = payload + `sizeof (msg_t)`, 빈 payload 포함
- delimiter·join·leave = `sizeof (msg_t)`만, payload 제외
- routing ID·credential은 일반 charge 대상
- 기본 LWM = `ceil (HWM / 2)`, 홀수·짝수 HWM 모두
- 기본값보다 작은 양수 hint는 채택, 같거나 크면 기본값 유지
- hint 0은 hint 없음, HWM 0은 제약할 LWM 없음
- HWM 이상인 hint는 `HWM - 1`로 clamp, 최솟값은 1 byte

## L2. Registry dual-ledger 잔재 삭제

Registry 전역 dual ledger는 queue-local counter로 대체됐는데 비계가 남아 있었다.

- registry atomic 6개(application current·provisional, completion current, completion
  pending messages, monitor current, deferred origin credit): 남은 용도가 생성자와 "항상 0인
  값이 0인지" 확인하는 소멸자 단언뿐이었다.
- `account_provisional_frame`의 lane 분기: application과 completion 갈래가 모두 비어 있고
  세 번째 lane이 세 번째 lane인지 확인하는 단언만 남아 있었다.
- `cancel_decoder_reservations_unlocked`: inline reservation은 metadata만 들고 있어 no-op.
  호출부 두 곳과 함께 제거했다.

실제로 무언가를 확인하는 소멸자 단언(`_directions` 비었는지, 살아 있는 lease·peak counter,
reserved minimum)은 남겼다.

## 실행 결과

- `test_flow_state_c_api` 13 tests 단독 10회 → 10/10 (handshake 수정 뒤 누적 27회 무결)
- `unittest_pipe_byte_charge` 7 tests 단독 10회 → 10/10
- Focused 8 + flow test 3개 + `unittest_poller` + `test_timer_poller` →
  `100% tests passed, 0 tests failed out of 14`
- 전체 sweep 91개 → **90 passed**. 유일한 실패는 `contract_public_surface`이고 원인은
  `zlink_socket_set_receive_flow_state`가 정식 spec에 없다는 기존 gap이다(보호 경로라 이번
  범위 밖). 이번에 추가한 세 flag macro는 이 검사에 걸리지 않는다 — 검사 대상은 함수와 export
  symbol이다.
- 이전 회차의 기존 실패 3건(`test_xpub_nodrop`, `test_router_multiple_dealers`,
  `test_zmp_metadata`)은 다른 작업자가 고쳐 이제 통과한다.
- `git diff --check` 통과.
