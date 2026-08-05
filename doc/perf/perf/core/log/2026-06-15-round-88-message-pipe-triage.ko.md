# Round 88: message/pipe 64B hot path 후보 검토

## 목표

64B one-way 처리량에 직접 영향을 주는 `msg_t` allocation/refcount와 pipe enqueue/dequeue 경로에서
POSD-safe 후보가 남아 있는지 확인한다. 완료 기준은 기존 helper와 계약을 확인하고, 이미 실패한 후보를
반복하지 않으며, 적용 가능한 최소 변경이 없으면 source를 수정하지 않는 것이다.

## 기준 report

- May26 full 보정 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 최신 reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`

## 확인한 코드

- `core/src/runtime/core/msg.hpp`
  - `msg_t::max_vsm_size`는 `msg_t` public/private 크기 제약 안에서 결정된다.
  - 64B payload는 VSM에 들어가지 않고 LMSG/refcount 경로를 탄다.
- `core/src/runtime/core/msg.cpp`
  - `init_size()`는 LMSG에서 `malloc(sizeof(content_t) + size)`를 사용한다.
  - `close()`와 `rm_refs()`는 LMSG content를 직접 `free()`한다.
  - slice view 전용 thread-local pool은 있지만 일반 LMSG storage에는 적용되지 않는다.
- `core/src/runtime/core/pipe.cpp`
  - `write_single_message_and_flush_no_recursive_hwm_check()`는 STREAM current-send 계열에서 이미 사용된다.
  - generic `write_and_flush()`는 routing-id와 multipart 의미를 보존해야 한다.
- `core/src/runtime/sockets/internal/lb.cpp`
  - 단일 active pipe fast path는 이미 있다.
  - final single message helper를 DEALER/PUBSUB 쪽에 적용한 후보는 round1에서 개선 없이 악화되어 되돌렸다.
- `core/src/runtime/sockets/internal/dist.cpp`
  - PUB/XPUB single-subscriber fast path와 VSM branch는 이미 있다.
  - HWM prechecked write 계열 후보는 round46에서 SPOT interop 계약을 깨서 폐기했다.

## 병목 가설

1. 64B LMSG allocation/refcount가 one-way 처리량의 주요 비용이다. 일반 LMSG content pool을 넣으면
   allocation 비용을 줄일 수 있다.
2. LMSG storage pool은 message ownership/lifetime의 새 정책이므로 복잡도가 크고, cross-thread close,
   custom free function, shared refcount, zcmsg/slice view와 충돌 위험이 있다. 작은 성능 후보로 다루기
   어렵다.
3. pipe final single helper는 분기 수를 줄일 수 있지만, 이전 targeted perf에서 DEALER/PUBSUB/SPOT 모두
   개선되지 않았다.

## POSD 판단

- 일반 LMSG pool은 `msg_t` 내부에 새 저장소 정책을 추가한다. 이는 단순 hot path 분기 제거가 아니라
  message lifetime 설계 변경이다.
- 이 변경은 `close()`, `rm_refs()`, `init_data()`, custom `ffn`, shared refcount와 함께 설계해야 하므로
  이번 라운드의 최소 변경 후보가 아니다.
- pipe final single helper는 이미 실측에서 악화된 후보라 반복하지 않는다.
- 따라서 이번 round에서는 source를 수정하지 않는다.

## 판정

- message/pipe 경로에서 바로 적용할 POSD-safe 후보는 찾지 못했다.
- 다음 후보는 SPOT data-plane에서 아직 남아 있는 복사/queue 경계가 현재 코드에서 실제로 반복되는지
  다시 확인한다. 다만 ownership 변경은 실패나 5% 이상 반복 개선 근거 없이는 남기지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: source 변경이 없다.
- 추가로 실행한 회귀 테스트: 없음
