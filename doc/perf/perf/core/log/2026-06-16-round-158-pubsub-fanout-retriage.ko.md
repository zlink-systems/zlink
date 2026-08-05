# Round 158: PUBSUB fanout 재분석

## 목표

- 현재 retained 상태에서 문제 report 대비 전체 64B 평균은 올랐지만 중앙값은 목표에 못 미친다.
- 중앙값을 끌어내리는 PUBSUB tls/ws/wss 경로를 다시 좁힌다.
- perf runner/client/server는 수정하지 않는다.

## 기준

- 과거 기준:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 사용자 보정 기준:
  - May26 smoke:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - May26 full:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 문제 report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 retained full64:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064431_round156_retained_spot_final_fastpath_full64_refresh.txt`

## 현재 retained 상태

- core source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()` 단일 FINAL fast path.
- 작업 전 `git diff --check`: 통과.

## 문제 report 대비 round156

- 공통 64B:
  - count: `26`
  - mean: `+8.46%`
  - median: `+5.17%`
  - `+10%` 초과 항목: `7`
  - `-10%` 미만 항목: `0`
- one-way 64B:
  - count: `10`
  - mean: `+7.58%`
  - median: `+1.31%`
- echo 64B:
  - count: `16`
  - mean: `+9.01%`
  - median: `+7.69%`
- 목표 미달:
  - 공통 64B 중앙값 `+10%` 미달.
  - one-way 64B 평균 `+10%` 미달.

## 낮은 개선 항목

- `MULTI_PUBSUB/tls/64`: `2,446,707.8 -> 2,418,113.6`, `-1.17%`
- `MULTI_PUBSUB/wss/64`: `2,679,903.2 -> 2,673,778.0`, `-0.23%`
- `MULTI_PUBSUB/ws/64`: `2,220,372.2 -> 2,218,658.0`, `-0.08%`
- `MULTI_PUBSUB/tcp/64`: `2,628,104.8 -> 2,724,922.6`, `+3.68%`

## 병목 가설

1. PUBSUB fanout에서 matching 결과가 단순한 경우에도 `dist_t`가 매 메시지마다
   per-pipe 상태를 반복 확인한다. 64B one-way에서는 transport보다 fanout loop와
   message refcount/copy가 중심 비용일 수 있다.
2. TLS/WSS에서 ASIO gather/write 정책이 PUBSUB fanout의 burst shape와 맞지 않아
   전송 측 batching 또는 write 활성화 비용이 커졌을 수 있다.
3. `zlink_publish_part()` helper 우회는 과거 round9/42/75에서 하락 또는 무효로
   확인됐으므로 이번 라운드 후보에서 제외한다.

## 먼저 검증할 가설

- 가설 1: `dist_t`와 PUB/SUB matching/fanout hot path를 코드 기준으로 다시 읽고,
  단순 fanout에서 하락 없는 최소 변경 후보가 있는지 확인한다.

## 코드 확인

읽은 파일:

- `core/src/runtime/sockets/pubsub/xpub.cpp`
- `core/src/runtime/sockets/pubsub/xpub.hpp`
- `core/src/runtime/sockets/pubsub/pub.cpp`
- `core/src/runtime/sockets/pubsub/pub.hpp`
- `core/src/runtime/sockets/internal/dist.cpp`
- `core/src/runtime/sockets/internal/dist.hpp`
- `core/src/runtime/utils/generic_mtrie_impl.hpp`
- `core/src/runtime/utils/mtrie.hpp`
- `core/src/runtime/core/pipe.cpp`
- `core/src/runtime/core/pipe.hpp`
- `core/src/runtime/engine/asio/asio_engine.cpp`
- `core/src/runtime/engine/asio/i_asio_transport.hpp`

확인 내용:

- PUB hot path는 `xpub_t::xsend()`에서 topic frame을 `_subscriptions.match()`로
  matching pipe에 매핑하고, `_dist.check_hwm()` 뒤 `_dist.send_to_matching()`으로
  fanout한다.
- `dist_t`에는 이미 아래 최적화가 있다.
  - single matching pipe fast path
  - VSM fanout 분기
  - matching HWM cache
- 64B payload는 VSM이 아니므로 LMSG/refcount fanout 경로를 탄다.
- `pipe_t`에는 final single message helper가 있지만, 일반 `dist_t`/`lb_t`로 확장한
  후보는 round144와 round99에서 하락 항목이 있어 원복됐다.
- `generic_mtrie_t` 삭제/방문 경로의 비재귀 구조는 보안 하드닝 항목이므로
  recursion 기반 shortcut이나 깊은 prefix cache를 추가하지 않는다.

## 기존 후보 재검토

- `zlink_publish_part()` 단일 FINAL fast path:
  - round9/42/75에서 하락 또는 무효로 원복.
- XPUB repeated-topic matching cache:
  - topic/topology invalidation 상태가 늘고, 단일 tcp 개선 신호가 작았다.
  - full/current 기준 명확한 하락 없는 개선으로 확인되지 않아 배제 유지.
- mtrie `match_with()` functor overload:
  - round72에서 테스트는 통과했지만 `PUBSUB/tls`가 낮아져 원복.
- dist `msg_more` 재사용:
  - round82에서 상승은 1% 안팎이고 tcp 하락이 있어 원복.
- dist final single-message pipe helper:
  - round99와 round144에서 하락 항목이 있어 원복.
- TLS `async_write_some()` 변경:
  - round80에서 `PUBSUB/tls` 하락으로 원복.
- ASIO handler allocator 범위 확대:
  - round106에서 WSS 일부 상승을 SPOT_SENDSEND/REQREP 하락이 상쇄해 원복.
- TLS tiny gather:
  - round136에서는 신호가 있었지만 round139에서 PUBSUB/tls 개선이 사라지고
    STREAM 하락 리스크가 있어 원복.

## POSD 판단

- 현재 PUBSUB gap은 한 줄짜리 중복 제거보다 fanout/matching/message lifetime/transport output이
  겹친 비용으로 보인다.
- 반복 topic cache나 empty-subscription 상태처럼 새 상태를 추가하는 방식은
  invalidation 지식을 `xpub_t`에 더 많이 노출한다. 기존 실측도 하락 없는 개선을 보이지 않았다.
- LMSG pool이나 VSM 확대는 message storage 정책을 바꾸는 큰 설계 변경이다.
- 따라서 이번 라운드에서는 새 source 변경을 적용하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. source 변경 없이 코드 triage만 수행했다.
- 보안 의미를 유지한 근거:
  - mtrie 비재귀화, WS/WSS pending-copy 제거, 포트 파싱, IPC unlink 순서,
    decoder/message/send guard, `maxmsgsize` 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - 없음. 새 source 변경이 없다.

## 판정

- 이번 라운드는 source 변경 없음.
- retained source diff는 `zlink_spot_send_spot_part()` 단일 FINAL fast path 하나로 유지한다.
- 전체 목표는 아직 미달이다.
  - 문제 report 대비 공통 64B 평균은 `+8.46%`로 목표를 넘었지만 중앙값은 `+5.17%`다.
  - one-way 64B 평균은 `+7.58%`로 목표 `+10%`에 못 미친다.
- 다음 후보는 새 상태 추가보다, retained SPOT_SENDSEND fast path를 유지할지/원복할지
  엄격 기준으로 재판정하거나, 실패/회귀 없이 남길 수 있는 구조 개선을 별도 분리하는 것이다.
