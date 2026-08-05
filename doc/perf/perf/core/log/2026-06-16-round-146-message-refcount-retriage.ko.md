# Round 146: message/refcount retriage

## 목표

- 전체 64B one-way와 PUBSUB fanout에 영향을 주는 `msg_t` allocation/refcount 경로를 다시 확인한다.
- 과거에 하락 항목 때문에 원복한 small LMSG pool과 pipe single-message helper를 반복하지 않는다.
- 새 저장소 정책이나 ABI/layout 변경 없이 적용 가능한 core hot path 중복만 찾는다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round142 one-way current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_042836_round142_oneway_targeted_current.txt`
- round144 rejected dist/lb candidate:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_045424_round144_dist_lb_single_message_recheck.txt`
- round145 STREAM/tcp current:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_051333_round145_stream_tcp_current_lowload_retry.txt`

## 시작 상태

- 유지 source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path
- round144 `dist/lb` single-message helper 후보는 원복했다.
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 수정하지 않는다.

## 병목 가설

1. 64B payload는 현재 VSM 범위를 넘어 LMSG/refcount 경로를 탄다. PUBSUB/SPOT fanout에서는
   `add_refs()`와 pipe write가 반복 비용을 만든다.
2. 하지만 일반 LMSG pool, VSM 한계 확대, shared refcount 정책 변경은 message lifetime 설계를 바꾸므로
   POSD 비용과 ABI/계약 위험이 크다.
3. 남은 가능성은 저장소 정책 변경이 아니라 `init_size()`, `copy()`, `move()`, `close()` 주변의 작은
   중복 제거다. 이 후보도 call path와 테스트 근거가 없으면 적용하지 않는다.

## 기존 후보 재검토

- small LMSG block pool:
  - round10에서 failure 0은 유지했지만 `SPOT/tls -19.14%`, `SPOT/ws -19.60%` 등 큰 하락이 있었다.
  - message lifetime 정책을 추가하므로 그대로 반복하지 않는다.
- pipe final single-message helper:
  - round144에서 `PUBSUB/tcp +5.50%`는 있었지만 `SPOT/tls -3.82%`, `SPOT/wss -1.14%`가 나와 원복했다.
- VSM 확대:
  - `zlink_msg_t`/`msg_t` layout과 routing/group 필드 의미를 건드리므로 이번 성능 라운드 후보가 아니다.

## 먼저 검증할 가설

- `msg_t` 구현을 읽고, 저장소 정책 변경 없이 제거 가능한 hot path 중복이 있는지 확인한다.
- 후보가 없으면 source를 수정하지 않고 다음은 mailbox/wakeup 쪽으로 넘어간다.

## 코드 확인

읽은 파일:

- `core/src/runtime/core/msg.hpp`
- `core/src/runtime/core/msg.cpp`
- `core/src/runtime/core/pipe.cpp`

확인 내용:

- `msg_t::max_vsm_size`는 현재 layout에서 64B보다 작다. 64B payload는 일반 LMSG 경로를 탄다.
- `msg_t::init_size()`의 LMSG 경로는 `content_t + payload`를 한 번에 할당한다.
- `msg_t::copy()`는 LMSG/ZCMSG를 공유 상태로 만들고 refcount를 올린다.
- `msg_t::add_refs()`는 fanout 전용 대량 refcount 추가를 이미 한 번에 처리한다.
- `pipe_t::write_and_flush()` 계열은 `more` 값을 이미 한 번만 계산하고 flush까지 같은 lock 안에서 수행한다.
- `write_single_message_and_flush_no_recursive_hwm_check()`는 존재하지만, round144에서 일반 dist/lb 경로에
  확장하면 하락 항목이 생겼다.

## POSD 판단

- 일반 LMSG pool은 allocation 비용을 줄일 수 있어 보여도 `close()`, `rm_refs()`, `init_data()`, custom
  free function, slice view storage까지 함께 건드리는 새 저장소 정책이다.
- VSM 한계 확대는 public `zlink_msg_t`와 private `msg_t` layout의 균형을 바꾸는 큰 변경이다.
- LMSG refcount fanout을 줄이는 것은 PUBSUB/SPOT fanout의 소유권과 HWM failure rollback 의미에 닿는다.
- 현재 코드에서 저장소 정책을 추가하지 않고 제거할 수 있는 명확한 중복은 찾지 못했다.

## 판정

- 이번 round에서는 source를 수정하지 않는다.
- message/refcount 경로는 성능 병목 후보이지만, 안전한 최소 변경 후보가 아니다.
- 다음 후보는 mailbox/wakeup과 poller batch 경로에서 작은 메시지 wakeup 비용을 다시 확인한다.

## mailbox/wakeup 추가 확인

읽은 파일:

- `core/src/runtime/core/mailbox.cpp`
- `core/src/runtime/core/mailbox.hpp`
- `core/src/runtime/core/io_thread.cpp`
- `core/src/runtime/core/object.cpp`
- `core/src/runtime/core/pipe.cpp`

확인 내용:

- `mailbox_t::send()`는 command pipe flush가 sleeping을 반환할 때만 signal과 ASIO post를 수행한다.
- `mailbox_t::schedule_if_needed()`는 `_scheduled` atomic으로 중복 post를 병합한다.
- `io_thread_t::process_mailbox()`는 EAGAIN까지 command를 drain하고, 남은 command가 있으면 reschedule한다.
- `pipe_t::flush_unlocked()`도 `_out_pipe->flush()`가 sleeping을 반환할 때만 `activate_read`를 보낸다.
- `send_activate_write()`의 same-thread 직접 처리는 이미 있고, `send_activate_read()` 직접 처리 후보는
  round26/64 계열에서 성능 개선을 만들지 못했다.

판단:

- mailbox/wakeup 경로에는 단순 중복 wakeup 제거 후보가 남아 있지 않다.
- 같은 thread `activate_read` 직접 처리는 재진입 위험이 있고, 기존 실측도 좋지 않았다.
- mailbox scheduling 정책을 바꾸는 것은 `_active`, `_scheduled`, cpipe ownership 의미를 바꾸는
  concurrency contract 변경이므로 이번 성능 후보로 적용하지 않는다.
- 다음 확인 대상은 poller/ASIO read-write batching이다.

## ASIO read/write batching 추가 확인

읽은 파일:

- `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp`
- `core/src/runtime/engine/asio/asio_engine.cpp`

확인 내용:

- STREAM read/write target, gather threshold, speculative write/read 정책은
  `asio_stream_fastpath_policy.hpp`에 모여 있다.
- `prepare_output_buffer()`와 `process_output()`의 encoder fill loop 중복 제거 후보는 round86에서
  tests는 통과했지만 `PUBSUB/tcp`, `STREAM/tcp`, `STREAM/tls`가 낮아져 원복됐다.
- STREAM batch size, initial target cap, auto-HWM throughput profile probe는 round121/128에서
  실패 또는 개선 없음으로 배제됐다.
- TLS/STREAM tiny gather 계열은 round136~139에서 인접 패턴 하락 때문에 원복됐다.
- read drain/speculative write 정책은 이미 STREAM 전용으로 모여 있고, 단순 기본값 변경은 기존 probe에서
  400kops gap을 해결하지 못했다.

판단:

- ASIO batch 정책은 이미 정보 은닉 관점에서 한 모듈에 모여 있다.
- 하락 없는 작은 개선으로 재채택할 후보는 현재 없다.
- 다음 확인 대상은 PUB/SUB matching/send-all 경로다.

## PUB/SUB matching/send-all 추가 확인

읽은 파일:

- `core/src/runtime/sockets/pubsub/xpub.cpp`
- `core/src/runtime/sockets/pubsub/pub.cpp`
- `core/src/runtime/sockets/internal/dist.cpp`
- `core/src/runtime/sockets/internal/dist.hpp`

확인 내용:

- 일반 PUBSUB hot path는 `_subscriptions.match()`로 matching pipe를 고른 뒤
  `_dist.check_hwm()`와 `_dist.send_to_matching()`으로 내려간다.
- `_send_all_data` 경로는 별도 internal option이며, 현재 core/perf 경로에서 직접 쓰는 호출자는 보이지 않는다.
- `dist_t`에는 이미 다음 최적화가 있다.
  - 단일 matching pipe fast path
  - VSM fanout 분기
  - matching HWM cache
- mtrie callback 제거 후보는 round72에서 효과가 없었다.
- empty subscription relaxed load 후보는 round79에서 `+0.22%` 수준이라 원복됐다.
- empty subscription active pipe 상태 후보는 이전 재분류에서 `ws -5.19%` 하락 때문에 배제됐다.

판단:

- PUB/SUB matching/send-all 경로에서 하락 없는 작은 후보가 현재 남아 있지 않다.
- `send_all_data`의 HWM 순서를 건드리는 것은 성능 후보가 아니라 의미 변경 후보에 가깝고, 이번 목표의
  직접 hot path 근거도 약하다.
- 이번 round는 source 변경 없이 종료한다.

## Round 146 최종 판단

- message/refcount, mailbox/wakeup, ASIO batching, PUB/SUB matching을 다시 확인했다.
- 기존에 원복한 후보를 재채택할 근거는 없었다.
- 새 core source 변경은 하지 않는다.
- retained source diff는 `zlink_spot_send_spot_part()` 단일 FINAL fast path 하나로 유지한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. source 변경 없이 코드 triage만 수행했다.
- 보안 의미를 유지한 근거:
  - decoder/message/send guard, `maxmsgsize` 정책, WS/WSS pending-copy 제거, mtrie 비재귀화,
    포트 파싱, IPC unlink 순서를 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - 없음. source 변경 없음.
