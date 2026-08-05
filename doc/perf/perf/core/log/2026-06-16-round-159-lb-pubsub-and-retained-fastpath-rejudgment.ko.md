# Round 159: LB/PUBSUB 재검토와 retained fast path 재판정

## 목표

- 사용자 보정 기준인 May26 smoke/full을 기준으로 현재 retained 상태를 다시 본다.
- 1-2% 개선이라도 하락 항목 없이 플러스면 채택 가능하다는 기준을 적용하되,
  POSD 위반이나 새 상태 복잡도 증가가 더 크면 채택하지 않는다.
- perf runner/client/server와 benchmark 조건은 수정하지 않는다.

## 기준

- May26 smoke:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- retained full64:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_064431_round156_retained_spot_final_fastpath_full64_refresh.txt`
- SPOT_SENDSEND same-window A/B:
  `doc/plan/perf/core/log/2026-06-16-round-131-spot-sendsend-fastpath-final-ab.ko.md`

## 현재 retained source 상태

- source diff는 하나만 남아 있다.
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path.
- `git diff --check -- core/src core/include core/tests`: 통과.

## LB hot path 재검토

읽은 파일:

- `core/src/runtime/sockets/internal/lb.cpp`
- `core/src/runtime/sockets/internal/lb.hpp`
- `core/src/runtime/sockets/dealer/dealer.cpp`
- `core/src/runtime/sockets/router/router_send_path.cpp`
- `core/src/runtime/sockets/common/socket_base_routing.cpp`
- `doc/plan/perf/core/log/2026-06-15-round-83-pipe-lb-hotpath-triage.ko.md`

확인 내용:

- `lb_t::sendpipe()`에는 이미 단일 active pipe fast path가 있다.
- weighted scheduling은 `ZLINK_ROUTER_OPT_WEIGHT`, `ZLINK_DEALER_OPT_WEIGHT` 계약과 연결되어 있다.
- default weight에서 map/schedule 비용을 더 줄이려면 pipe weight 상태를 중복 저장하거나
  별도 플래그를 늘려야 한다.

판단:

- DEALER 단일 pipe benchmark에는 기존 fast path가 이미 적용된다.
- PUBSUB에는 LB가 직접 적용되지 않는다.
- weighted 상태를 더 늘리는 변경은 정보 중복과 invalidation 지식을 추가한다.
- 이번 라운드에서 LB source 변경은 하지 않는다.

## PUBSUB fanout 재검토

읽은 파일:

- `core/src/runtime/sockets/pubsub/xpub.cpp`
- `core/src/runtime/sockets/pubsub/pub.cpp`
- `core/src/runtime/sockets/internal/dist.cpp`
- `doc/plan/perf/core/log/2026-06-16-round-158-pubsub-fanout-retriage.ko.md`
- `doc/plan/perf/core/log/2026-06-16-round-146-message-refcount-retriage.ko.md`
- `doc/plan/perf/core/log/2026-06-16-round-135-pubsub-spot-worst-recheck.ko.md`

확인 내용:

- PUB hot path는 `_subscriptions.match()` 뒤 `_dist.check_hwm()`와
  `_dist.send_to_matching()`으로 내려간다.
- `dist_t`에는 이미 single matching pipe fast path, VSM fanout 분기, matching HWM cache가 있다.
- 64B payload는 VSM이 아니라 LMSG/refcount fanout 경로를 탄다.
- 기존 dist/mtrie/TLS 후보들은 하락 항목 때문에 원복됐다.

판단:

- PUBSUB의 남은 낮은 항목은 단일 중복 제거보다 fanout, message lifetime, transport output이 겹친 비용이다.
- topic cache, empty-subscription 상태, LMSG pool, VSM 확대는 새 상태나 저장소 정책을 늘린다.
- 하락 없는 플러스 실측이 없는 상태에서 POSD 비용을 감수하지 않는다.
- 이번 라운드에서 PUBSUB source 변경은 하지 않는다.

## retained SPOT_SENDSEND fast path 재판정

round131 same-window A/B:

| transport | retained | removed | delta |
|-----------|---------:|--------:|------:|
| tcp | 265740.0 | 255675.6 | +3.94% |
| tls | 253885.0 | 246771.0 | +2.88% |
| ws | 245004.4 | 241593.2 | +1.41% |
| wss | 256716.2 | 256475.4 | +0.09% |

round125 same-window A/B도 `tcp +3.70%`, `tls +6.19%`, `wss +2.81%`였다.

판단:

- 5% 이상 개선은 일부 transport에만 해당하지만, same-window A/B에서 하락 항목 없이 모두 플러스다.
- 변경 범위는 public API와 wire format을 바꾸지 않고, 단일 FINAL send의 내부 staged sequence 비용만 줄인다.
- 실패 시 caller part를 consume하는 기존 소유권 의미도 유지한다.
- 따라서 사용자 기준인 "하락 항목 없이 +면 채택 가능"과 POSD 기준을 모두 만족하는 retained 변경으로 유지한다.

## May26 기준 해석

- round156 retained full64에서 STREAM/tcp 64B는 May26 full 대비 `+13.25%`다.
- STREAM/ws도 `+21.07%`, STREAM/tls도 `+8.53%`다.
- STREAM/wss는 `+3.74%`로 플러스지만 5% 미만이다.
- May26 full 대비 하락 항목은 `SPOT/wss`, `PUBSUB/tls`, `PUBSUB/wss`,
  `SPOT_SENDSEND/tcp`에 남아 있다.
- 이 하락들은 retained fast path의 직접 대상이 아니거나, same-window 원복 A/B에서는 retained가 더 높았다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. source 변경 없이 코드와 로그를 재검토했다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending-copy 제거, mtrie 비재귀화, port parsing, IPC unlink 순서,
    decoder/message/send guard, `maxmsgsize` 정책을 변경하지 않는다.

## 판정

- 새 source 변경 없음.
- retained source diff는 `zlink_spot_send_spot_part()` 단일 FINAL fast path 하나로 유지한다.
- LB/PUBSUB 쪽은 POSD-safe하고 하락 없는 실측 후보가 현재 없다.
- 다음 단계는 현재 retained 상태로 failure gate와 최종 diff/로그 정리를 진행하거나,
  추가 후보가 필요하면 STREAM/wss처럼 아직 플러스지만 5% 미만인 항목을 별도 대상으로 좁힌다.
